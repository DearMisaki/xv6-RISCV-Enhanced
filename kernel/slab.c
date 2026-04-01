#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "list.h"
#include "slab.h"

// 声明 kalloc.c 中定义的结构和工具函数
struct page {
    int order;
    int is_free;
    struct slab *slab;
    struct list_node node;
};

// 引入 kalloc.c 里的地址转换工具
extern struct page* pa2page(uint64 pa);
extern uint64 page2pa(struct page* p);

// 预定义几个常用大小的缓存
#define MAX_CACHES 7
struct kmem_cache kmalloc_caches[MAX_CACHES];
int cache_sizes[MAX_CACHES] = {32, 64, 128, 256, 512, 1024, 2048};

void kmem_cache_init(struct kmem_cache *cache, char *name, int size) {
    initlock(&cache->lock, name);
    safestrcpy(cache->name, name, sizeof(cache->name));
    
    // 对齐到 8 字节
    cache->obj_size = (size + 7) & ~7;

    if (cache->obj_size >= 2048) cache->order = 1;

    cache->order = 0; // 默认分配一页 (4KB)
    cache->objs_per_slab = (PGSIZE << cache->order) / cache->obj_size;
    
    list_init(&cache->slabs_full);
    list_init(&cache->slabs_partial);
    list_init(&cache->slabs_free);

    // [新增] 初始化 Per-CPU 锁和链表
    for(int i = 0; i < NCPU; i++) {
        initlock(&cache->cpu[i].lock, name);
        cache->cpu[i].freelist = 0;
        cache->cpu[i].count = 0;    }
}

void slab_init() {
    for (int i = 0; i < MAX_CACHES; i++) {
        char name[16];
        // 不能用 sprintf，我们简单手写个名字
        name[0] = 'k'; name[1] = 'm'; name[2] = '-';
        name[3] = '0' + cache_sizes[i] / 1000 % 10;
        name[4] = '0' + cache_sizes[i] / 100 % 10;
        name[5] = '0' + cache_sizes[i] / 10 % 10;
        name[6] = '0' + cache_sizes[i] % 10;
        name[7] = '\0';
        kmem_cache_init(&kmalloc_caches[i], name, cache_sizes[i]);
    }
}

// 供 kfree_ext 使用的全局底层释放
void kmem_cache_free_global(struct kmem_cache *cache, void *obj) {
    acquire(&cache->lock);
    struct page *p = pa2page((uint64)obj);
    struct slab *s = p->slab;

    if (!s) panic("kmem_cache_free_global: invalid slab");

    *(void**)obj = s->freelist;
    s->freelist = obj;
    s->inuse--;
    s->free_count++;

    // 释放后 slab 全满
    if (s->inuse == 0) {
        list_del(&s->list);
        
        int num_pages = 1 << cache->order;
        for (int i = 0; i < num_pages; i++) {
            struct page *pg = pa2page((uint64)s->base_page + i * PGSIZE);
            pg->slab = 0;
        }
        
        free_pages((void *)page2pa(s->base_page), cache->order);
    } else if (s->free_count == 1) {
        list_del(&s->list);
        list_add_tail(&cache->slabs_partial, &s->list);
    }
    release(&cache->lock);
}

void* kmem_cache_alloc(struct kmem_cache *cache) {
    push_off();
    int id = cpuid();
    acquire(&cache->cpu[id].lock);

    // 1. [新增] 极速路径：从 CPU 本地链表直接取
    if (cache->cpu[id].freelist != 0) {
        void *obj = cache->cpu[id].freelist;
        cache->cpu[id].freelist = *(void**)obj;
        cache->cpu[id].count--;
        release(&cache->cpu[id].lock);
        pop_off();
        memset(obj, 0, cache->obj_size);
        return obj;
    }
    release(&cache->cpu[id].lock);
    pop_off();

    // 2. 慢速路径：从全局获取对象
    acquire(&cache->lock);
    struct slab *s = 0;

    if (!list_empty(&cache->slabs_partial)) {
        s = list_entry(cache->slabs_partial.next, struct slab, list);
    } else if (!list_empty(&cache->slabs_free)) {
        s = list_entry(cache->slabs_free.next, struct slab, list);
        list_del(&s->list);
        list_add_tail(&cache->slabs_partial, &s->list);
    } else {
        void *pa = alloc_pages(cache->order);
        if (!pa) {
            release(&cache->lock);
            return 0; 
        }

        s = (struct slab*)pa;
        s->base_page = pa2page((uint64)pa);
        
        int num_pages = 1 << cache->order;
        for (int i = 0; i < num_pages; i++) {
            struct page *pg = pa2page((uint64)pa + i * PGSIZE);
            pg->slab = s;
        }
        s->cache = cache; 
        s->inuse = 0;
        
        int offset = (sizeof(struct slab) + 7) & ~7;
        s->free_count = ((PGSIZE << cache->order) - offset) / cache->obj_size;
        
        s->freelist = (char*)pa + offset;
        void **curr = (void**)s->freelist;
        for (int i = 0; i < s->free_count - 1; i++) {
            *curr = (char*)curr + cache->obj_size;
            curr = (void**)*curr;
        }
        *curr = 0; 
        list_add_tail(&cache->slabs_partial, &s->list);
    }

    void *obj = s->freelist;
    s->freelist = *(void**)obj; 
    s->inuse++;
    s->free_count--;

    if (s->free_count == 0) {
        list_del(&s->list);
        list_add_tail(&cache->slabs_full, &s->list);
    }

    release(&cache->lock);
    memset(obj, 0, cache->obj_size);
    return obj;
}

void kmem_cache_free(struct kmem_cache *cache, void *obj) {
    if(!obj) return;
    memset(obj, 1, cache->obj_size);

    push_off();
    int id = cpuid();
    acquire(&cache->cpu[id].lock);

    // 1. [新增] 极速释放：挂入 CPU 本地链表
    *(void**)obj = cache->cpu[id].freelist;
    cache->cpu[id].freelist = obj;
    cache->cpu[id].count++;

    // 2. [新增] 阈值控制：如果本地存货太多（比如16个），上交一半回全局 Slab
    if (cache->cpu[id].count > 16) {
        void *flush_list = cache->cpu[id].freelist;
        void *tail = flush_list;
        
        // 遍历找到第 8 个对象切断链表
        for(int i = 1; i < 8; i++) {
            tail = *(void**)tail;
        }
        
        cache->cpu[id].freelist = *(void**)tail; // 本地留下另一半
        *(void**)tail = 0;                       // 切断
        cache->cpu[id].count -= 8;
        
        release(&cache->cpu[id].lock);
        pop_off();

        // 遍历被切下来的那一半，逐个真正归还给全局的 Slab
        void *curr = flush_list;
        while(curr) {
            void *next = *(void**)curr;
            kmem_cache_free_global(cache, curr);
            curr = next;
        }
        return;
    }

    release(&cache->cpu[id].lock);
    pop_off();
}

void kfree_ext(void* ptr) {
    if (!ptr) return;
    struct page *p = pa2page((uint64)ptr);
    if (!p->slab) panic("kfree_ext: not a slab object");
    
    kmem_cache_free(p->slab->cache, ptr);
}

void* kmalloc(uint64 size) {
    if (size > 2048) panic("kmalloc: size too large"); // 超过2KB建议直接用 alloc_pages

    for (int i = 0; i < MAX_CACHES; i++) {
        if (size <= cache_sizes[i]) {
            return kmem_cache_alloc(&kmalloc_caches[i]);
        }
    }
    return 0;
}
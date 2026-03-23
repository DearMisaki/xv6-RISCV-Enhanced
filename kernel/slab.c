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
    
    cache->order = 0; // 默认分配一页 (4KB)
    if (cache->obj_size >= 2048) cache->order = 1; // 简单的阶数调整
    
    cache->objs_per_slab = (PGSIZE << cache->order) / cache->obj_size;
    
    list_init(&cache->slabs_full);
    list_init(&cache->slabs_partial);
    list_init(&cache->slabs_free);
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

// ====== 1. 分配逻辑修改：记录 Cache 归属 ======
void* kmem_cache_alloc(struct kmem_cache *cache) {
    acquire(&cache->lock);
    struct slab *s = 0;

    // 三个链表 full partial free 能够使寻找空闲块的时间复杂度从 O(N) 变成 O(1)
    // 如果只使用一个全局链表，由于不同的通过指针串在一起，要寻找到一个空闲块，
    // 首先要遍历一个个slab，然后再遍历slab里面
    // 但是存在这个三个链表的话，我们只用在partial中取出头节点就好了
    // 关键点，在于释放的顺序和分配的顺序不同
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
            return 0; // OOM
        }

        // 将分配的这些页，变成一个slab
        // 在起始位置写入元数据
        s = (struct slab*)pa;
        s->base_page = pa2page((uint64)pa);
        // 分配的这 n 页需要指定他们的归属权

        // int num_pages = 1 << cache->order;
        // for (int i = 0; i < num_pages; i++) {
        //     struct page *pg = pa2page((uint64)pa + i * PGSIZE);
        //     pg->slab = s;
        // }
        s->base_page->slab = s;
        s->cache = cache; // [新增] 明确记录这个 Slab 属于哪个 Cache!
        s->inuse = 0;
        
        // 元数据偏移
        int offset = (sizeof(struct slab) + 7) & ~7;
        // 一个cache最多可以装多少个 obj
        s->free_count = ((PGSIZE << cache->order) - offset) / cache->obj_size;
        
        s->freelist = (char*)pa + offset;
        void **curr = (void**)s->freelist;
        for (int i = 0; i < s->free_count - 1; i++) {
            // 计算出下一个对象的地址，并且写到当前块的前 8 个字节
            *curr = (char*)curr + cache->obj_size;
            // 移动到下一个块的开头
            curr = (void**)*curr;
        }
        *curr = 0; 

        list_add_tail(&cache->slabs_partial, &s->list);
    }

    void *obj = s->freelist;
    // 同样的方式，将头节点移动到下一个块
    s->freelist = *(void**)obj; 
    s->inuse++;
    s->free_count--;

    if (s->free_count == 0) {
        list_del(&s->list);
        list_add_tail(&cache->slabs_full, &s->list);
    }

    release(&cache->lock);
    return obj;
}

// ====== 2. 释放逻辑修改：修正状态转移漏洞 ======
void kmem_cache_free(struct kmem_cache *cache, void *obj) {
    acquire(&cache->lock);

    // 从物理地址拿到 page index，再从buddy中拿到对应的 page 对象
    struct page *p = pa2page((uint64)obj);
    // 在 kmem_cache_alloc 中，为每一页指定了
    struct slab *s = p->slab;

    if (!s) panic("kmem_cache_free: invalid slab");

    *(void**)obj = s->freelist;
    s->freelist = obj;
    s->inuse--;
    s->free_count++;

    // [修正] 必须先判断 inuse == 0 (全空)，再判断 free_count == 1 (部分空)
    if (s->inuse == 0) {
        // 从当前链表摘除，彻底归还给 Buddy System
        list_del(&s->list);
        s->base_page->slab = 0;
        free_pages((void *)page2pa(s->base_page), cache->order);
    } else if (s->free_count == 1) {
        // 刚从全满变回部分空闲
        list_del(&s->list);
        list_add_tail(&cache->slabs_partial, &s->list);
    }

    release(&cache->lock);
}

// ====== 3. kfree_ext 彻底精简 ======
void kfree_ext(void* ptr) {
    if (!ptr) return;
    struct page *p = pa2page((uint64)ptr);
    if (!p->slab) panic("kfree_ext: not a slab object");
    
    // [修正] 直接通过指针获取所属 Cache，彻底告别除法反查！
    kmem_cache_free(p->slab->cache, ptr);
}

// 通用 kmalloc 接口
void* kmalloc(uint64 size) {
    if (size > 2048) panic("kmalloc: size too large"); // 超过2KB建议直接用 alloc_pages

    for (int i = 0; i < MAX_CACHES; i++) {
        if (size <= cache_sizes[i]) {
            return kmem_cache_alloc(&kmalloc_caches[i]);
        }
    }
    return 0;
}
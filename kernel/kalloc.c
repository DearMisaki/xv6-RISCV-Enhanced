#include "types.h"
#include "param.h"     // [新增] 包含 NCPU
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "list.h"

extern char end[];

#define MAX_ORDER 11
#define PCP_HIGH_WATERMARK 64  // PCP 本地缓存上限
#define PCP_BATCH_SIZE 32      // 每次向全局上交或偷取的批量大小

struct slab;

struct page {
    int order;
    int is_free;            // 0:已分配, 1:全局空闲, 2:PCP本地空闲
    struct slab *slab;
    struct list_node node;
};

// [新增] 每 CPU 页缓存 (Per-CPU Pageset)
struct pcp_list {
    struct spinlock lock;
    struct list_node free_list;
    int count;
};

struct {
    struct spinlock lock;
    struct page *pages;
    struct list_node free_area[MAX_ORDER];
    uint64 page_count;
    uint64 base_addr;
    struct pcp_list pcp[NCPU]; // [新增]
} buddy;

void freerange(void *pa_start, void *pa_end);
void global_free_pages(void *pa, int order); // 原 free_pages 改名

struct page* pa2page(uint64 pa) {
    int idx = (pa - buddy.base_addr) / PGSIZE;
    return &buddy.pages[idx];
}

uint64 page2pa(struct page* p) {
    int idx = p - buddy.pages;
    return buddy.base_addr + idx * PGSIZE;
}

void kinit() {
    initlock(&buddy.lock, "kmem_global");

    for (int i = 0; i < NCPU; i++) {
        initlock(&buddy.pcp[i].lock, "kmem");
        list_init(&buddy.pcp[i].free_list);
        buddy.pcp[i].count = 0;
    }

    for (int i = 0; i < MAX_ORDER; i++) {
        list_init(&buddy.free_area[i]);
    }

    buddy.pages = (struct page*)PGROUNDUP((uint64)end);
    uint64 total_mem = PHYSTOP - (uint64)buddy.pages;
    buddy.page_count = total_mem / (PGSIZE + sizeof(struct page));
    buddy.base_addr = PGROUNDUP((uint64)(buddy.pages + buddy.page_count));

    for (int i = 0; i < buddy.page_count; i++) {
        buddy.pages[i].order = -1;
        buddy.pages[i].is_free = 0;
        buddy.pages[i].slab = 0;
        list_init(&buddy.pages[i].node);
    }

    freerange((void*)buddy.base_addr, (void*)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end) {
    char *p;
    p = (char*)PGROUNDUP((uint64)pa_start);
    for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
        free_pages(p, 0); // 直接调用表层 API 即可
}

// ====== 提取出的无锁核心，专供内部批处理使用 ======
void* _global_alloc_pages_nolock(int order) {
    struct page *p = 0;
    int current_order = order;
    while (current_order < MAX_ORDER && list_empty(&buddy.free_area[current_order])) {
        current_order++;
    }
    if (current_order == MAX_ORDER) return 0; 

    struct list_node *node = buddy.free_area[current_order].next;
    list_del(node);
    p = list_entry(node, struct page, node);
    p->is_free = 0;

    while (current_order > order) {
        current_order--;
        int page_idx = p - buddy.pages;
        int buddy_idx = page_idx + (1 << current_order); 
        struct page *buddy_page = &buddy.pages[buddy_idx];
        buddy_page->is_free = 1;
        buddy_page->order = current_order;
        list_add_tail(&buddy.free_area[current_order], &buddy_page->node);
    }
    p->order = order;
    return (void*)page2pa(p);
}

void _global_free_pages_nolock(void *pa_ptr, int order) {
    struct page *p = pa2page((uint64)pa_ptr);
    int page_idx = p - buddy.pages;
    p->is_free = 1;

    while (order < MAX_ORDER - 1) {
        int buddy_idx = page_idx ^ (1 << order);
        if (buddy_idx >= buddy.page_count) break;
        struct page *buddy_page = &buddy.pages[buddy_idx];

        if (buddy_page->is_free != 1 || buddy_page->order != order) break;

        list_del(&buddy_page->node);
        buddy_page->is_free = 0;
        buddy_page->order = -1;

        if (buddy_idx < page_idx) {
            page_idx = buddy_idx;
            p = buddy_page;
        }
        order++;
    }
    p->order = order;
    list_add_tail(&buddy.free_area[order], &p->node);
}

// ====== 原来的全局接口（包装一层锁，并处理内存清理） ======
void* global_alloc_pages(int order) {
    acquire(&buddy.lock);
    void *pa = _global_alloc_pages_nolock(order);
    release(&buddy.lock);
    if (pa) memset((char*)pa, 5, (1 << order) * PGSIZE); 
    return pa;
}

void global_free_pages(void *pa_ptr, int order) {
    uint64 pa = (uint64)pa_ptr;
    if(((pa % PGSIZE) != 0) || pa < buddy.base_addr || pa >= PHYSTOP)
        panic("global_free_pages");
    memset(pa_ptr, 1, (1 << order) * PGSIZE); // 在锁外执行耗时的 memset
    acquire(&buddy.lock);
    _global_free_pages_nolock(pa_ptr, order);
    release(&buddy.lock);
}

void * alloc_pages(int order) {
    if (order == 0) {
        push_off();
        int id = cpuid();
        
        // 1. 本地分配 (Fast Path)
        acquire(&buddy.pcp[id].lock);
        if (!list_empty(&buddy.pcp[id].free_list)) {
            struct list_node *node = buddy.pcp[id].free_list.next;
            list_del(node);
            buddy.pcp[id].count--;
            release(&buddy.pcp[id].lock);
            
            struct page *p = list_entry(node, struct page, node);
            p->is_free = 0;
            pop_off();
            uint64 pa = page2pa(p);
            memset((void*)pa, 5, PGSIZE);
            return (void*)pa;
        }
        release(&buddy.pcp[id].lock);

        // 2. 优先从全局 Buddy 批量进货
        // 【核心优化】无锁窥探：如果全局也是空的，绝不去碰那把极度拥挤的 buddy.lock！
        int global_has_free = 0;
        for(int i = 0; i < MAX_ORDER; i++) {
            if(!list_empty(&buddy.free_area[i])) {
                global_has_free = 1;
                break;
            }
        }

        if (global_has_free) {
            acquire(&buddy.lock);
            void *first_pa = _global_alloc_pages_nolock(0);
            
            if (first_pa) {
                // 既然拿到了全局锁，就一次性多拿一批放进本地
                acquire(&buddy.pcp[id].lock); 
                for (int i = 0; i < PCP_BATCH_SIZE - 1; i++) {
                    void *extra_pa = _global_alloc_pages_nolock(0);
                    if (!extra_pa) break;
                    struct page *p = pa2page((uint64)extra_pa);
                    p->is_free = 2; 
                    p->order = 0;
                    list_add_tail(&buddy.pcp[id].free_list, &p->node);
                    buddy.pcp[id].count++;
                }
                release(&buddy.pcp[id].lock);
            }
            release(&buddy.lock);

            if (first_pa) {
                pop_off();
                memset(first_pa, 5, PGSIZE);
                return first_pa;
            }
        }

        // 3. 全局也没货了，最后绝招：去偷其它 CPU 的 (Steal)
        // 必须加锁窥探，以保证 test2 不漏掉一页；但因为偷窃顺序延后，触发次数大幅减少
        for (int i = 0; i < NCPU; i++) {
            if (i == id) continue;
            
            acquire(&buddy.pcp[i].lock); 
            if (buddy.pcp[i].count > 0) { 
                // 【核心优化】既然来偷了，就全部洗劫一空 (Steal All)，防止频繁多方争夺
                struct list_node stolen_list;
                list_init(&stolen_list);
                
                while (buddy.pcp[i].count > 0) {
                    struct list_node *node = buddy.pcp[i].free_list.next;
                    list_del(node);
                    buddy.pcp[i].count--;
                    list_add_tail(&stolen_list, node);
                }
                release(&buddy.pcp[i].lock);

                // 把赃物存入自己的口袋
                acquire(&buddy.pcp[id].lock); 
                while (!list_empty(&stolen_list)) {
                    struct list_node *node = stolen_list.next;
                    list_del(node);
                    list_add_tail(&buddy.pcp[id].free_list, node);
                    buddy.pcp[id].count++;
                }

                // 拿出一个自己用
                struct list_node *node = buddy.pcp[id].free_list.next;
                list_del(node);
                buddy.pcp[id].count--;
                release(&buddy.pcp[id].lock);

                struct page *p = list_entry(node, struct page, node);
                p->is_free = 0;
                pop_off();
                
                uint64 pa = page2pa(p);
                memset((void*)pa, 5, PGSIZE);
                return (void*)pa;
            }
            release(&buddy.pcp[i].lock);
        }

        pop_off();
        return 0; // 彻彻底底的 OOM
    }
    
    return global_alloc_pages(order);
}

void free_pages(void *pa_ptr, int order) {
    if (order == 0) {
        // 先填充垃圾数据，以便尽早捕获 UAF
        memset(pa_ptr, 1, PGSIZE);
        
        push_off();
        int id = cpuid();
        acquire(&buddy.pcp[id].lock);

        struct page *p = pa2page((uint64)pa_ptr);
        p->is_free = 2; // [关键] 标记为 2，表明它在 PCP 中，全局 Buddy 不能对其进行伙伴合并
        p->order = 0;

        list_add_tail(&buddy.pcp[id].free_list, &p->node);
        buddy.pcp[id].count++;

        // 如果本地缓存太多，上交一批给全局
        if (buddy.pcp[id].count > PCP_HIGH_WATERMARK) {
            struct list_node flush_list;
            list_init(&flush_list);
            
            for (int i = 0; i < PCP_BATCH_SIZE; i++) {
                struct list_node *node = buddy.pcp[id].free_list.next;
                list_del(node);
                buddy.pcp[id].count--;
                list_add_tail(&flush_list, node);
            }
            release(&buddy.pcp[id].lock);

            // 【优化3：全局批量上交 (Lock Batching)】
            // 只拿一次全局锁，把这 32 个页面一口气全部还回去
            acquire(&buddy.lock); 
            while (!list_empty(&flush_list)) {
                struct list_node *node = flush_list.next;
                list_del(node);
                struct page *fp = list_entry(node, struct page, node);
                fp->is_free = 0; 
                _global_free_pages_nolock((void*)page2pa(fp), 0);
            }
            release(&buddy.lock);
        } else {
            release(&buddy.pcp[id].lock);
        }
        
        pop_off();
        return;
    }

    global_free_pages(pa_ptr, order);
}

void *kalloc(void) { return alloc_pages(0); }

void kfree(void *pa) { free_pages(pa, 0); }
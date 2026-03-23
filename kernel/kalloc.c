// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages or 2^order pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "list.h"  // 必须包含我们之前创建的双向链表头文件

#define MAX_ORDER 11

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct slab; // 前向声明，供给 slab 分配器使用

// 物理页描述符 (取代了原来的 struct run)
// 这个结构体存放在内存的前端，不占用实际的分配页
struct page {
    int order;              // 当前页块的阶数，-1表示已被分配
    int is_free;            // 1 表示空闲，0 表示已分配
    struct slab *slab;      // 指向所属的 slab (用于 slab 分配器)
    struct list_node node;  // 用于挂载到 free_area 链表
};

// 伙伴系统全局状态 (取代了原来的 kmem)
struct {
    struct spinlock lock;
    struct page *pages;                    // 页描述符数组的首地址
    struct list_node free_area[MAX_ORDER]; // 多阶空闲双向链表数组
    uint64 page_count;                     // 总物理页数
    uint64 base_addr;                      // 真正可以用来分配的物理内存起始地址
} buddy;

void freerange(void *pa_start, void *pa_end);
void free_pages(void *pa, int order);

// 工具函数：根据物理地址计算它对应的 page 结构体
struct page* pa2page(uint64 pa) {
    int idx = (pa - buddy.base_addr) / PGSIZE;
    return &buddy.pages[idx];
}

// 工具函数：根据 page 结构体计算它的物理地址
uint64 page2pa(struct page* p) {
    int idx = p - buddy.pages;
    return buddy.base_addr + idx * PGSIZE;
}

void
kinit()
{
    initlock(&buddy.lock, "kmem");

    // 初始化多阶空闲链表
    for (int i = 0; i < MAX_ORDER; i++) {
        list_init(&buddy.free_area[i]);
    }

    // 1. 划分元数据空间
    // 我们把所有的 struct page 数组紧贴着内核代码结束(end)的地方存放
    buddy.pages = (struct page*)PGROUNDUP((uint64)end);
    
    // 计算除去页描述符数组本身后，剩余的可用内存
    uint64 total_mem = PHYSTOP - (uint64)buddy.pages;
    buddy.page_count = total_mem / (PGSIZE + sizeof(struct page));
    
    // 真正的可分配内存起始地址，buddy.pages + buddy.page_count 计算元数据存放的结束位置
    buddy.base_addr = PGROUNDUP((uint64)(buddy.pages + buddy.page_count));

    // 2. 初始化所有的页描述符
    for (int i = 0; i < buddy.page_count; i++) {
        buddy.pages[i].order = -1;
        buddy.pages[i].is_free = 0;
        buddy.pages[i].slab = 0;
        list_init(&buddy.pages[i].node);
    }

    // 3. 把剩余的可用内存全部交给伙伴系统
    freerange((void*)buddy.base_addr, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
    char *p;
    p = (char*)PGROUNDUP((uint64)pa_start);
    // 逐页释放，依靠 free_pages 内部的逻辑自动将它们合并成大块 (高阶块)
    for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
        free_pages(p, 0);
}

// 核心分配逻辑：分配 2^order 个连续物理页
void *
alloc_pages(int order)
{
    struct page *p = 0;
    acquire(&buddy.lock);

    // 寻找满足条件的最小空闲块
    int current_order = order;
    while (current_order < MAX_ORDER && list_empty(&buddy.free_area[current_order])) {
        current_order++;
    }

    if (current_order == MAX_ORDER) {
        release(&buddy.lock);
        return 0; // Out of memory
    }

    // 从链表中取下该块
    struct list_node *node = buddy.free_area[current_order].next;
    list_del(node);
    p = list_entry(node, struct page, node);
    p->is_free = 0;

    // 向下分裂，直到达到请求的阶数
    while (current_order > order) {
        current_order--;
        int page_idx = p - buddy.pages;
        int buddy_idx = page_idx + (1 << current_order); // 找到伙伴块的索引
        
        struct page *buddy_page = &buddy.pages[buddy_idx];
        buddy_page->is_free = 1;
        buddy_page->order = current_order;
        // 将分裂出来的一半挂到较低阶的空闲链表上
        list_add_tail(&buddy.free_area[current_order], &buddy_page->node);
    }

    p->order = order;
    release(&buddy.lock);

    uint64 pa = page2pa(p);
    // 保持原来的习惯，分配时填充垃圾数据
    memset((char*)pa, 5, (1 << order) * PGSIZE); 
    return (void*)pa;
}

// 核心释放逻辑：释放块并尝试与空闲伙伴合并
void
free_pages(void *pa_ptr, int order)
{
    uint64 pa = (uint64)pa_ptr;

    // 边界与对齐检查 (注意这里用 buddy.base_addr 替代了原来的 end)
    if(((pa % PGSIZE) != 0) || pa < buddy.base_addr || pa >= PHYSTOP)
        panic("free_pages: bad pa");

    // 保持原来的习惯，释放时填充垃圾数据以捕获悬垂引用
    memset(pa_ptr, 1, (1 << order) * PGSIZE);

    acquire(&buddy.lock);

    struct page *p = pa2page(pa);
    int page_idx = p - buddy.pages;

    p->is_free = 1;

    // 尝试向上合并伙伴块
    while (order < MAX_ORDER - 1) {
        // 利用异或运算快速找到当前阶的伙伴块索引
        int buddy_idx = page_idx ^ (1 << order);
        
        // 越界保护
        if (buddy_idx >= buddy.page_count) break;

        struct page *buddy_page = &buddy.pages[buddy_idx];

        // 如果伙伴块不是空闲的，或者阶数不同，则无法合并，跳出循环
        if (!buddy_page->is_free || buddy_page->order != order) {
            break;
        }

        // 可以合并！将伙伴块从空闲链表中摘下
        list_del(&buddy_page->node);
        buddy_page->is_free = 0;
        buddy_page->order = -1;

        // 合并后，新块的起始索引是两者的最小值
        if (buddy_idx < page_idx) {
            page_idx = buddy_idx;
            p = buddy_page;
        }
        order++;
    }

    // 将最终合并得到的大块挂入对应的空闲链表
    p->order = order;
    list_add_tail(&buddy.free_area[order], &p->node);

    release(&buddy.lock);
}

// ====== 兼容原版 xv6 的接口 ======

// 原版 kalloc 分配单页
void *
kalloc(void)
{
    return alloc_pages(0);
}

// 原版 kfree 释放单页
void
kfree(void *pa)
{
    free_pages(pa, 0);
}
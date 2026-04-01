#ifndef SLAB_H
#define SLAB_H

#include "param.h"
#include "types.h"
#include "list.h"
#include "spinlock.h"

struct slab {
    struct page *base_page;  
    int inuse;               
    int free_count;          
    void *freelist;          
    struct list_node list;   
    struct kmem_cache *cache; // [新增] 指向该 Slab 所属的 Cache
};

// [新增] Slab Per-CPU 结构
struct kmem_cache_cpu {
    struct spinlock lock;
    void *freelist;  // 本地直接维护一个对象的单向链表
    int count;       // 当前缓存的对象数量
};

struct kmem_cache {
    struct spinlock lock;
    char name[16];
    int obj_size;            // 对齐后的对象大小
    int order;               // 需要向 Buddy 申请的阶数
    int objs_per_slab;       // 每个 slab 的对象总数

    struct list_node slabs_full;
    struct list_node slabs_partial;
    struct list_node slabs_free;

    struct kmem_cache_cpu cpu[NCPU]; // NCPU 通常为 8 [新增]
};

// 全局 API
void slab_init(void);
void* kmalloc(uint64 size);
void kfree_ext(void* ptr); // 注意：为了避免与按页释放的 kfree 冲突，命名为 kfree_ext

#endif
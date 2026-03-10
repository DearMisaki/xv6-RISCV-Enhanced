// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

struct 
{
  struct spinlock lock;
  int cnt[PHYSTOP / PGSIZE];
} pg_ref;

int kpgrefcnt(void *pa)
{
  return pg_ref.cnt[(uint64)pa / PGSIZE];
}

int kaddpgref(void* pa)
{
  if ((uint64)pa % PGSIZE != 0 || (char*)pa < end || (uint64)pa > PHYSTOP)
    return -1;

  acquire(&pg_ref.lock);

  ++pg_ref.cnt[(uint64)pa / PGSIZE];

  release(&pg_ref.lock);

  return 0;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&pg_ref.lock, "pg_ref");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
  {
    // 系统启动时才会执行 freerange，所以手动赋值为 1，方便 kfree 减 1
    pg_ref.cnt[(uint64)p / PGSIZE] = 1;
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&pg_ref.lock);

  // 引用计数为 0 再清空
  if (--pg_ref.cnt[(uint64)pa / PGSIZE] == 0)
  {
    release(&pg_ref.lock);

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run*)pa;

    acquire(&kmem.lock);

    r->next = kmem.freelist;

    kmem.freelist = r;

    release(&kmem.lock);
  }
  else
  {
    release(&pg_ref.lock);
  }
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
  {
    kmem.freelist = r->next;
    release(&kmem.lock);

    acquire(&pg_ref.lock);
    pg_ref.cnt[(uint64)r / PGSIZE] = 1;
    release(&pg_ref.lock);
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

int cowpage(pagetable_t pagetable, uint64 va)
{
  if (va >= MAXVA)
    return -1;
  
  pte_t *pte = walk(pagetable, va, 0);

  if (pte == 0)
    return -1;

  if ((*pte & PTE_V) == 0)
    return -1;

  return (*pte & PTE_COW ? 1 : 0);
}

void* cowalloc(pagetable_t pagetable, uint64 va)
{
  if (va % PGSIZE != 0)
    return 0;

  uint64 pa = walkaddr(pagetable, va);

  if (pa == 0)
    return 0;

  pte_t* pte = walk(pagetable, va, 0);

  if (kpgrefcnt((char *)pa) == 1)
  {
    *pte |= PTE_W;
    *pte &= ~PTE_COW;

    return (void*)pa;
  }
  else
  {
    char* mem = kalloc();

    if (mem == 0)
      return 0;

    memmove(mem, (char*)pa, PGSIZE);

    // *pte &= ~PTE_V;

    // if (mappages(pagetable, va, PGSIZE, mem, (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW) != 0)
    // {
    //   kfree(mem);
    //   *pte |= PTE_V;

    //   return 0;
    // }

    uint flags = PTE_FLAGS(*pte);

    flags = (flags | PTE_W) & ~PTE_COW;

    *pte = PA2PTE(mem) | flags | PTE_V;

    kfree((void*)PGROUNDDOWN(pa));

    return mem;
  }
}
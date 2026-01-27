#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

extern uint64 phystop;

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, phystop-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  
  return kpgtbl;
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// 将全局页表重新写回 stap 寄存器
void kpgtbl2satp(void)
{
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  sfence_vma();
}

// 拷贝进程页表到进程独立的内核页表
int u2kvmcopy(pagetable_t upagtbl, pagetable_t kpagtbl, uint64 start, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  // 对齐向下
  start = PGROUNDUP(start);
  
  for(i = start; i < start + sz; i += PGSIZE){
    // 获取用户页表的 PTE
    if((pte = walk(upagtbl, i, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;
    
    pa = PTE2PA(*pte);
    
    // 获取原有标志位，但必须关闭 PTE_U
    // 因为 RISC-V S模式下，默认不能访问设置了 U 位的页
    flags = PTE_FLAGS(*pte) & (~PTE_U);

    // 映射到内核页表
    if(mappages(kpagtbl, i, PGSIZE, pa, flags) != 0)
      goto err;
  }
  return 0;

err:
  // 发生错误时，应该取消之前的映射 (简略版可直接 panic)
  uvmunmap(kpagtbl, start, (i-start)/PGSIZE, 0);
  return -1;
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];

    if(*pte & PTE_V) {
      // --- 关键修正：精确识别大页 ---
      // 只有在 Level 1，且 R/W/X 位被设置时，才作为大页返回。
      // 如果只是 V=1 但没有权限位，说明它是指向下一级的目录，必须继续向下走。
      if(level == 1 && (*pte & (PTE_R|PTE_W|PTE_X))) {
          return pte;
      }
      
      // 更新 pagetable 为下一级页表的物理地址
      pagetable = (pagetable_t)PTE2PA(*pte);
      
    } else {
      // 如果不存在且 alloc=0，返回 null
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      
      memset(pagetable, 0, PGSIZE);
      
      // 建立新的目录项：指向下一级页表
      // 注意：这里只设置 V，不设置 R/W/X
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  
  // 走到 Level 0，返回最终的 PTE
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}


#if defined(LAB_PGTBL) || defined(SOL_MMAP) || defined(SOL_COW)
void
vmprint_recursive(pagetable_t pagetable, int level, uint64 va)
{
  for (int i = 0; i < 512; ++i)
  {
    pte_t pte = pagetable[i];

    // 条目有效
    if (pte & PTE_V)
    {
      uint64 current_va = va | ((uint64)i << (level * 9 + 12));

      uint64 pa = PTE2PA(pte);

      for (int i = 0; i < 3 - level; ++i)
      {
        printf(" ..");
      }

      printf("0x%lx: pte 0x%lx pa 0x%lx\n", current_va, pte, pa);

      // 不是叶子节点，进入下一级页表
      if ((pte & (PTE_R | PTE_W | PTE_X)) == 0) {
        pagetable_t next_pt = (pagetable_t)pa;
        if (level > 0) {
          vmprint_recursive(next_pt, level - 1, current_va);
        }
      }
    }
  }
}

void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);
  vmprint_recursive(pagetable, 2, 0);
}
#endif



// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// 映射一个大页
int
mappages_super(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
uint64 a, last;
  pte_t *pte;

  // 1. 参数检查：必须是 2MB 对齐
  if(size == 0)
    panic("mappages_super: size");
  
  if(va % SUPERPGSIZE != 0)
    panic("mappages_super: va not aligned");

  if(pa % SUPERPGSIZE != 0)
    panic("mappages_super: pa not aligned");

  a = va;
  last = va + size;

  for(;;){
    // --- 手动 Walk 第一层 (Level 2) ---
    // 获取 Level 2 的 PTE（根页表项）
    pte = &pagetable[PX(2, a)];

    // 如果 Level 2 条目无效（即还没有分配对应的 Level 1 页表）
    if(!(*pte & PTE_V)){
      // 分配一个新的物理页作为 Level 1 页表
      pagetable_t new_l1_table = (pagetable_t)kalloc();
      if(new_l1_table == 0)
        return -1; // 内存分配失败
      
      memset(new_l1_table, 0, PGSIZE);
      
      // 让 Level 2 的 PTE 指向这个新分配的 Level 1 页表
      // 注意：这里是目录项，只需要 PTE_V，不需要 R/W/X
      *pte = PA2PTE(new_l1_table) | PTE_V;
    }

    // --- 查找/设置 Level 1 (目标层级) ---
    // 获取 Level 1 页表的地址
    pagetable_t l1_pagetable = (pagetable_t)PTE2PA(*pte);
    
    // 获取 Level 1 的 PTE 指针
    pte = &l1_pagetable[PX(1, a)];

    // 检查是否已经存在映射 (Remap)
    if(*pte & PTE_V)
      panic("mappages_super: remap");

    // --- 核心步骤：写入大页映射 ---
    // 这里的 pa 是 2MB 物理大块的起始地址
    // perm 包含了 PTE_R/W/X/U 等标志
    // PTE_V 标记该条目有效
    // 关键：由于 PTE_R/W/X 中至少有一位被设置，硬件会知道这是叶子节点（大页）
    *pte = PA2PTE(pa) | perm | PTE_V;

    // --- 循环控制 ---
    a += SUPERPGSIZE;
    pa += SUPERPGSIZE;
    
    if(a == last)
      break;
  }
  
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
// kernel/vm.c

// kernel/vm.c

void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    pte = walk(pagetable, a, 0);
    if(pte == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");

    // --- 区分大页与普通页的核心逻辑 ---
    
    // 1. 获取 Level 2 的 PTE
    pte_t *pte_l2 = &pagetable[PX(2, a)];
    
    // 2. 计算出 Level 1 的 PTE 地址
    // 注意：我们需要确保 L2 是指向下一级的，但既然 walk 成功了，这肯定没问题
    pagetable_t pgtbl_l1 = (pagetable_t)PTE2PA(*pte_l2);
    pte_t *pte_l1 = &pgtbl_l1[PX(1, a)];

    // 3. 判断 walk 返回的是否就是 Level 1 的 PTE
    // 如果 walk 返回的指针地址 == 我们计算出的 Level 1 PTE 地址
    // 并且它有权限位，那它才是真正的大页！
    int is_super = (pte == pte_l1) && (*pte & (PTE_R|PTE_W|PTE_X));

    if (is_super) {
      // ===========================
      //      大页处理逻辑
      // ===========================
      if ((a % SUPERPGSIZE == 0) && (npages - (a-va)/PGSIZE >= 512)) {
        // 方案 A: 整体释放
        uint64 pa = PTE2PA(*pte);
        if(do_free){
          superfree((void*)pa);
        }
        *pte = 0;
        a += SUPERPGSIZE - PGSIZE; 
        continue;
      } else {
        // 方案 B: 拆分 (Demote)
        // ... (保持你之前的拆分代码不变) ...
        
        // 1. 保存旧的大页物理地址和权限
        uint64 old_pa = PTE2PA(*pte);
        int old_perm = PTE_FLAGS(*pte);
        
        // 2. 分配一个新的 Level 0 页表
        char *new_pt = kalloc();
        if(new_pt == 0) panic("uvmunmap: kalloc demote");
        memset(new_pt, 0, PGSIZE);
        
        // 3. 填充新页表
        pte_t *level0_pt = (pte_t*)new_pt;
        for(int i = 0; i < 512; i++){
          level0_pt[i] = PA2PTE(old_pa + i*PGSIZE) | old_perm | PTE_V;
        }
        
        // 4. 更新 L1 PTE 指向新页表 (Directory, No Perms)
        *pte = PA2PTE(new_pt) | PTE_V;
        
        sfence_vma(); 
        
        // 重新 walk 获取 Level 0 的 PTE
        pte = walk(pagetable, a, 0);
      }
    }

    // ===========================
    //      普通页处理逻辑
    // ===========================
    // 此时 pte 指向的是 Level 0 的叶子节点
    // 或者 demote 之后重新 walk 得到的 Level 0 节点
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// 确保你有 SUPERPGSIZE 定义，通常在 kernel/riscv.h
#ifndef SUPERPGSIZE
#define SUPERPGSIZE (2L * 1024 * 1024)
#endif

uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  int sz;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  
  // 循环步长由 sz 动态决定
  for(a = oldsz; a < newsz; a += sz){
    
    // --- 尝试使用大页 ---
    // 条件：
    // 1. 当前地址 a 是 2MB 对齐的
    // 2. 剩余需要的空间 (newsz - a) 足够放下 2MB
    if((a % SUPERPGSIZE == 0) && (a + SUPERPGSIZE <= newsz)){
      mem = superalloc(); // 尝试分配大页
      if(mem != 0){
        sz = SUPERPGSIZE; // 成功！本次循环步进 2MB
        
        // 初始化内存 (大页通常不清零，但为了安全建议清零，或者依赖分配器的初始化)
        memset(mem, 0, sz); 

        // 调用我们之前写的 mappages_super
        if(mappages_super(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
          superfree(mem);
          uvmdealloc(pagetable, a, oldsz); // 清理已分配的部分
          return 0;
        }
        
        // 成功处理完大页，直接进入下一次循环
        continue; 
      }
      // 如果 superalloc 返回 0（大页用光了），则 Fallback 到下面的普通页逻辑
    }

    // --- 普通页逻辑 (4KB) ---
    sz = PGSIZE;
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    
#ifndef LAB_SYSCALL
    memset(mem, 0, sz);
#endif

    if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      // backtrace();
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;
  int szinc;

  for(i = 0; i < sz; i += szinc){
    szinc = PGSIZE; 

    if((pte = walk(old, i, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;

    // ==========================================
    // 关键修正：准确识别大页 (Level 1 Leaf)
    // ==========================================
    int is_super_page = 0;
    
    // 1. 获取 Level 2 PTE
    pte_t *pte_l2 = &old[PX(2, i)];
    if(*pte_l2 & PTE_V) {
        // 2. 计算 Level 1 PTE 的地址
        pagetable_t pgtbl_l1 = (pagetable_t)PTE2PA(*pte_l2);
        pte_t *pte_l1 = &pgtbl_l1[PX(1, i)];
        
        // 3. 只有当 walk 返回的 pte 指针就是 Level 1 的指针，且具有权限时，才是大页
        if (pte == pte_l1 && (*pte & (PTE_R|PTE_W|PTE_X))) {
            is_super_page = 1;
        }
    }

    if (is_super_page) {
        // 只有确认为大页，且地址对齐时，才执行大页拷贝
        if (i % SUPERPGSIZE == 0) {
            pa = PTE2PA(*pte);
            flags = PTE_FLAGS(*pte);

            if((mem = superalloc()) == 0) {
                goto err;
            }
            memmove(mem, (char*)pa, SUPERPGSIZE);

            if(mappages_super(new, i, SUPERPGSIZE, (uint64)mem, flags) != 0){
                superfree(mem);
                goto err;
            }
            szinc = SUPERPGSIZE; 
            continue; 
        }
    }
    // ==========================================

    // 普通页逻辑 (保持不变)
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;

    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    if((pte = walk(pagetable, va0, 0)) == 0) {
      // printf("copyout: pte should exist %lx %ld\n", dstva, len);
      return -1;
    }


    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
    
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
// 进程双页表版本 
// 替换原有的 copyin
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 limit = PLIC; // 必须限制在 PLIC 以下，防止覆盖设备映射

  if(srcva >= limit || srcva + len > limit || srcva + len < srcva)
    return -1;

  // 直接拷贝！利用当前的 satp (即 p->kpagetable)
  memmove((void*)dst, (void*)srcva, len);
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
// 替换原有的 copyinstr
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 limit = PLIC;
  uint64 i;
  char *p = (char *)srcva;

  for(i = 0; i < max; i++){
    if((uint64)(p + i) >= limit)
      return -1;
    
    dst[i] = p[i]; // 硬件地址翻译
    if(p[i] == '\0')
      return 0;
  }
  return -1;
}


// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();
  

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }

  if (mappages(p->kpagetable, va, PGSIZE, mem, PTE_W|PTE_R) != 0) {
     // 处理错误，如 undo 用户页表映射并 free mem
     uvmunmap(p->pagetable, va, 1, 0); 
     kfree((void *)mem);
     return 0;
  }
  
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va) {
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}



#ifdef LAB_PGTBL
pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}
#endif

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

volatile static int started = 0;

// [新增] 专门用于测试 Slab 和 Buddy 联动的函数
void slab_test() {
    printf("\n--- Starting Slab Allocator Test ---\n");

    // 1. 分配几个小对象 (触发 kmem_cache_alloc)
    void *p1 = kmalloc(32);
    void *p2 = kmalloc(32);
    // 分配一个大对象 (使用不同大小的 Cache)
    void *p3 = kmalloc(2048);
    void *p4 = kmalloc(2048);

    if(!p1 || !p2 || !p3) {
        panic("slab_test: kmalloc failed!");
    }

    printf("Allocated addresses:\n");
    printf("p1 (32b): %p\n", p1);
    printf("p2 (32b): %p\n", p2);
    printf("p3 (2048b): %p\n", p3);
    printf("p4 (2048b): %p\n", p4);

    // 2. 写入数据，测试内存是否真的可读写
    *(int*)p1 = 0xdeadbeef;
    *(int*)p3 = 0x12345678;
    *(int*)p4 = 0x12345678;


    // 3. 释放对象 (触发 kmem_cache_free)
    // 当我们释放 p3 时，因为它是这个 1024 字节 Cache 里唯一被分配的对象，
    // 释放后该 Slab 将变得完全空闲，从而精准触发我们刚才修复的 free_pages 代码逻辑！
    kfree_ext(p1);
    kfree_ext(p2);
    kfree_ext(p3);
    kfree_ext(p4);

    printf("--- Slab Allocator Test Passed! ---\n\n");
}

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if(cpuid() == 0){
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    kinit(); 
    slab_init();// physical page allocator
    slab_test();
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    procinit();      // process table
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
    virtio_disk_init(); // emulated hard disk
    userinit();      // first user process
    __sync_synchronize();
    started = 1;
  } else {
    while(started == 0)
      ;
    __sync_synchronize();
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
  }

  scheduler();        
}

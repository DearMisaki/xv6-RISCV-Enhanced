#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "fdt.h"

extern uint64 dtb_addr; // 来自 start.c
uint64 phy_mem_size = 128 * 1024 * 1024; // 默认值，万一解析失败

void
fdt_init()
{
  if (dtb_addr == 0) return;

  struct fdt_header *header = (struct fdt_header *)dtb_addr;
  if (be32_to_cpu(header->magic) != FDT_MAGIC) {
    printf("FDT: Invalid magic.\n");
    return;
  }

  // 获取结构块的偏移地址
  uint8 *p = (uint8 *)dtb_addr + be32_to_cpu(header->off_dt_struct);
  char *str_table = (char *)dtb_addr + be32_to_cpu(header->off_dt_strings);

  int inside_memory_node = 0;

  while (1) {
    // 读取 token (4字节对齐)
    // 注意：这里的指针移动需要非常小心 4 字节对齐
    uint32 token = be32_to_cpu(*((uint32 *)p));
    p += 4;

    if (token == FDT_END) break;

    if (token == FDT_BEGIN_NODE) {
      char *node_name = (char *)p;
      // 移动指针跳过名字，并对齐到 4 字节
      int len = strlen(node_name) + 1;
      p += len;
      if ((uint64)p % 4 != 0) p += (4 - ((uint64)p % 4));

      // 检查节点名字是否是 "memory@" 开头
      // 更严谨的做法是检查 device_type 属性，但 QEMU 的 memory 节点通常叫 memory@80000000
      if (strncmp(node_name, "memory@", 7) == 0) {
        inside_memory_node = 1;
      } else {
        inside_memory_node = 0;
      }
    } 
    else if (token == FDT_PROP) {
      uint32 len = be32_to_cpu(*((uint32 *)p)); p += 4;
      uint32 nameoff = be32_to_cpu(*((uint32 *)p)); p += 4;
      char *prop_name = str_table + nameoff;

      // 如果我们在 memory 节点内，且属性名为 "reg"
      if (inside_memory_node && strncmp(prop_name, "reg", 3) == 0) {
        // reg 属性通常是 [address, size] 组合
        // 在 64 位 RISC-V QEMU 中，通常是 2个 cell (地址) + 2个 cell (大小)
        // 也就是前 8 字节是起始地址，后 8 字节是大小
        // *严谨实现应该读取根节点的 #address-cells 和 #size-cells*
        // 这里简化处理，直接取第二个 64 位整数
        
        uint64 *val_ptr = (uint64 *)p;
        // uint64 addr = be64_to_cpu(val_ptr[0]); // 应该是 0x80000000
        uint64 size = be64_to_cpu(val_ptr[1]);
        
        phy_mem_size = size;
        printf("FDT: Found memory size: %lu MB\n", size / 1024 / 1024);
      }

      // 跳过属性数据，并对齐
      p += len;
      if ((uint64)p % 4 != 0) p += (4 - ((uint64)p % 4));
    }
    else if (token == FDT_END_NODE) {
      inside_memory_node = 0;
    }
    else if (token == FDT_NOP) {
      continue;
    }
  }
}
#include "types.h"

#define FDT_MAGIC 0xd00dfeed
#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009

struct fdt_header {
  uint32 magic;
  uint32 totalsize;
  uint32 off_dt_struct;
  uint32 off_dt_strings;
  uint32 off_mem_rsvmap;
  uint32 version;
  uint32 last_comp_version;
  uint32 boot_cpuid_phys;
  uint32 size_dt_strings;
  uint32 size_dt_struct;
};

// 辅助函数：大端转小端 (Big Endian to CPU)
static inline uint32 be32_to_cpu(uint32 x) {
  return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
         ((x & 0xFF0000) >> 8) | ((x & 0xFF000000) >> 24);
}

static inline uint64 be64_to_cpu(uint64 x) {
  uint64 hi = be32_to_cpu((uint32)(x >> 32));
  uint64 lo = be32_to_cpu((uint32)x);
  return (lo << 32) | hi;
}

void fdt_init();
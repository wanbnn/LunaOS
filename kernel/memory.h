#ifndef LUNA_MEMORY_H
#define LUNA_MEMORY_H
#include "pmm.h"
typedef struct {
    uint64_t firmware_usable, addressable_usable, free_bytes, allocated_bytes, reserved_bytes, above_4g_usable;
    uint32_t allocations, metadata_bytes;
    bool ready;
} MemoryStats;
void memory_init(void);
int memory_allocate(uint32_t bytes,uint32_t align,uint32_t mask,uint32_t boundary,
                      bool prefer_high,bool clear,PmmAllocation *);
int memory_release(const PmmAllocation *);
int memory_lookup(uint32_t address,PmmAllocation *);
void memory_stats(MemoryStats *);
void memory_status(char *,size_t);
int memory_test(uint32_t mebibytes,char *,size_t);
#endif

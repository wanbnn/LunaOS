#include "heap.h"
#include "memory.h"
#include "driver.h"
#include "task.h"
#define MAGIC 0x4c484541u
#define GRANULE (64u*1024u) /* Growth granularity, not a total heap limit. */
typedef struct Block { uint32_t bytes,available; struct Block *next; uint32_t magic; } Block;
typedef struct Arena {
    PmmAllocation allocation;
    struct Arena *next;
    Block *first;
    uint32_t owner,live_bytes,allocations;
} Arena;
#define ROUND16(n) (((n)+15u)&~(size_t)15u)
#define BLOCK_BYTES ROUND16(sizeof(Block))
#define ARENA_BYTES ROUND16(sizeof(Arena))
static Arena *arenas;
static void *claim(Arena *arena,Block *b,uint32_t n) {
    if (b->bytes>=n+BLOCK_BYTES+16) {
        Block *split=(void *)((uint8_t *)b+BLOCK_BYTES+n);
        *split=(Block){b->bytes-n-BLOCK_BYTES,1,b->next,MAGIC};
        b->next=split; b->bytes=n;
    }
    arena->live_bytes+=b->bytes; arena->allocations++;
    b->available=0; return (uint8_t *)b+BLOCK_BYTES;
}
void *heap_allocate(size_t bytes) { return heap_allocate_owned(bytes,task_memory_owner()); }
void *heap_allocate_owned(size_t bytes,uint32_t pid) {
    if (!pid) return 0;
    if (!bytes) bytes=1;
    if (bytes>UINT32_MAX-ARENA_BYTES-BLOCK_BYTES-PMM_PAGE-16) return 0;
    uint32_t wanted=ROUND16(bytes);
    uint32_t flags=drv_irq_save();
    for (Arena *a=arenas; a; a=a->next) if (a->owner==pid)
        for (Block *b=a->first; b; b=b->next) if (b->available && b->bytes>=wanted) {
            void *p=claim(a,b,wanted); drv_irq_restore(flags); return p;
        }
    drv_irq_restore(flags);
    uint32_t size=wanted+ARENA_BYTES+BLOCK_BYTES;
    if (size<GRANULE) size=GRANULE;
    size=(size+PMM_PAGE-1)&~(PMM_PAGE-1);
    PmmAllocation allocation={0};
    if (memory_allocate(size,PMM_PAGE,UINT32_MAX,0,true,false,&allocation)) return 0;
    Arena *arena=(void *)(uintptr_t)allocation.address;
    Block *block=(void *)((uint8_t *)arena+ARENA_BYTES);
    *block=(Block){size-ARENA_BYTES-BLOCK_BYTES,1,0,MAGIC};
    flags=drv_irq_save();
    *arena=(Arena){.allocation=allocation,.next=arenas,.first=block,.owner=pid}; arenas=arena;
    void *p=claim(arena,block,wanted); drv_irq_restore(flags); return p;
}
static Block *find(const void *p,Arena **owner) {
    uintptr_t address=(uintptr_t)p;
    for (Arena *a=arenas; a; a=a->next) {
        uintptr_t start=(uintptr_t)a;
        if (address<start+ARENA_BYTES+BLOCK_BYTES || address-start>=a->allocation.bytes) continue;
        for (Block *b=a->first; b; b=b->next)
            if ((const uint8_t *)b+BLOCK_BYTES==p && b->magic==MAGIC && !b->available) {
                if (owner) *owner=a;
                return b;
            }
        return 0;
    }
    return 0;
}
size_t heap_usable_size(const void *p) {
    if (!p) return 0;
    uint32_t flags=drv_irq_save(); Block *b=find(p,0); size_t n=b ? b->bytes : 0;
    drv_irq_restore(flags); return n;
}
int heap_release(void *p) {
    if (!p) return 0;
    uint32_t flags=drv_irq_save(); Arena *arena=0; Block *b=find(p,&arena);
    if (!b) { drv_irq_restore(flags); return -22; }
    arena->live_bytes-=b->bytes; arena->allocations--;
    b->available=1;
    for (Block *a=arena->first; a && a->next;)
        if (a->available && a->next->available &&
            (uint8_t *)a+BLOCK_BYTES+a->bytes==(uint8_t *)a->next) {
            a->bytes+=BLOCK_BYTES+a->next->bytes; a->next=a->next->next;
        } else a=a->next;
    if (arena->first->available && !arena->first->next) {
        Arena **link=&arenas;
        while (*link!=arena) link=&(*link)->next;
        *link=arena->next;
        PmmAllocation allocation=arena->allocation;
        drv_irq_restore(flags); return memory_release(&allocation);
    }
    drv_irq_restore(flags); return 0;
}

void *heap_resize(void *p,size_t bytes) {
    if (!p) return heap_allocate(bytes);
    if (!bytes) { heap_release(p); return 0; }
    uint32_t flags=drv_irq_save(); Arena *arena=0; Block *b=find(p,&arena);
    size_t old=b ? b->bytes : 0; uint32_t owner=arena ? arena->owner : 0;
    drv_irq_restore(flags);
    if (!b) return 0;
    if (old>=bytes) return p;
    void *next=heap_allocate_owned(bytes,owner);
    if (!next) return 0;
    memcpy(next,p,old);
    if (heap_release(p)) { heap_release(next); return 0; }
    return next;
}
static void stats(uint32_t owner,bool all,HeapStats *out) {
    if (!out) return;
    *out=(HeapStats){0};
    uint32_t flags=drv_irq_save();
    for (Arena *a=arenas; a; a=a->next) if (all || a->owner==owner) {
        out->committed_bytes+=a->allocation.bytes; out->live_bytes+=a->live_bytes;
        out->allocations+=a->allocations; out->arenas++;
    }
    drv_irq_restore(flags);
}
void heap_stats(uint32_t pid,HeapStats *out) { stats(pid,false,out); }
void heap_stats_all(HeapStats *out) { stats(0,true,out); }
int heap_release_owner(uint32_t pid) {
    if (!pid) return -22;
    for (;;) {
        uint32_t flags=drv_irq_save(); Arena **link=&arenas;
        while (*link && (*link)->owner!=pid) link=&(*link)->next;
        if (!*link) { drv_irq_restore(flags); return 0; }
        Arena *arena=*link; PmmAllocation allocation=arena->allocation;
        *link=arena->next;
        int rc=memory_release(&allocation);
        if (rc) { arena->next=*link; *link=arena; }
        drv_irq_restore(flags);
        if (rc) return rc;
    }
}

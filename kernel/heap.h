#ifndef LUNA_HEAP_H
#define LUNA_HEAP_H
#include <stddef.h>
#include <stdint.h>
/* Physical arenas are exclusive to one PID, grow on demand and are counted
 * once. Application backends still execute as supervisor kernel services;
 * PID ownership is not a user-mode heap mapping/security boundary. */
typedef struct { uint64_t committed_bytes, live_bytes; uint32_t allocations, arenas; } HeapStats;
void *heap_allocate(size_t);
void *heap_allocate_owned(size_t,uint32_t pid);
void *heap_resize(void *,size_t);
int heap_release(void *);
size_t heap_usable_size(const void *);
void heap_stats(uint32_t pid,HeapStats *);
void heap_stats_all(HeapStats *);
/* The owner must be stopped and its backend globals/callbacks cleared first. */
int heap_release_owner(uint32_t pid);
#endif

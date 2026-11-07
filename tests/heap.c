#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../kernel/heap.h"
#include "../kernel/memory.h"
static Pmm pool;
static uint32_t current_owner=1;
uint32_t task_memory_owner(void) { return current_owner; }
uint32_t drv_irq_save(void) { return 0; }
void drv_irq_restore(uint32_t flags) { (void)flags; }
int memory_allocate(uint32_t n,uint32_t align,uint32_t mask,uint32_t boundary,bool high,bool clear,PmmAllocation *out) {
    int rc=pmm_allocate(&pool,n,align,mask,boundary,high,out);
    if(!rc && clear) memset((void *)(uintptr_t)out->address,0,n);
    return rc;
}
int memory_release(const PmmAllocation *p) { return pmm_release(&pool,p); }
int main(void) {
    size_t length=128*1024*1024;
    void *region=mmap(0,length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
    assert(region!=MAP_FAILED && (uintptr_t)region+length<=UINT32_MAX);
    PmmRange map[]={{(uintptr_t)region,length,1,1}};
    size_t bytes=pmm_metadata_bytes(map,1);void *meta=calloc(1,bytes);assert(meta);
    assert(!pmm_init(&pool,map,1,0,0,meta,bytes));uint32_t before=pool.free_pages;
    void *large=heap_allocate(12*1024*1024);assert(large && heap_usable_size(large)>=12*1024*1024);
    memset(large,0xa5,12*1024*1024);
    void *items[1000];
    for(unsigned i=0;i<1000;i++){items[i]=heap_allocate(100+i*13);assert(items[i] && !((uintptr_t)items[i]&15));memset(items[i],i,100+i*13);}
    for(unsigned i=0;i<1000;i++){unsigned char *p=items[i];assert(p[0]==(unsigned char)i && p[99+i*13]==(unsigned char)i);}
    for(unsigned i=0;i<1000;i+=2)assert(!heap_release(items[i]));
    for(unsigned i=1;i<1000;i+=2)assert(!heap_release(items[i]));
    assert(!heap_release(large));assert(heap_release(large)==-22);
    assert(!heap_allocate(SIZE_MAX) && pool.free_pages==before && !pool.allocation_count);
    current_owner=11;
    unsigned char *a=heap_allocate(17); assert(a); memset(a,0x75,17);
    HeapStats one,two,all; heap_stats(11,&one);
    assert(one.allocations==1 && one.live_bytes>=17 && one.committed_bytes>=65536);
    current_owner=22;
    void *b=heap_allocate(21); assert(b); heap_stats(22,&two); heap_stats_all(&all);
    assert(two.allocations==1 && all.committed_bytes==one.committed_bytes+two.committed_bytes);
    assert((uintptr_t)a/65536!=(uintptr_t)b/65536); /* PID-exclusive physical arenas */
    a=heap_resize(a,1024*1024); assert(a && a[0]==0x75 && a[16]==0x75);
    heap_stats(11,&one); HeapStats unchanged; heap_stats(22,&unchanged);
    assert(one.allocations==1 && one.live_bytes>=1024*1024 && unchanged.committed_bytes==two.committed_bytes);
    assert(!heap_resize(a,SIZE_MAX) && a[0]==0x75); /* failure preserves old allocation */
    assert(!heap_release(a)); heap_stats(11,&one); assert(!one.committed_bytes && !one.allocations);
    assert(!heap_release(b));
    current_owner=33;
    for(unsigned i=0;i<100;i++) assert(heap_allocate(8192));
    heap_stats(33,&one); assert(one.allocations==100);
    assert(!heap_release_owner(33)); heap_stats(33,&one); assert(!one.committed_bytes);
    current_owner=44; a=heap_allocate(31);assert(a);
    assert(!heap_release_owner(33));heap_stats(44,&one);assert(one.allocations==1);
    assert(!heap_release_owner(44));
    heap_stats_all(&all); assert(!all.committed_bytes && pool.free_pages==before);
    assert(!munmap(region,length));free(meta);
    puts("PASS: growable heap >8 MiB, alignment, fragmentation/coalescing, PID ownership/stats, realloc attribution, teardown and full arena reclamation");
}

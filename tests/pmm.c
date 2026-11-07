#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/pmm.h"
#define MIB UINT64_C(1048576)
static void test_map(uint64_t ram) {
    /* Holes, overlaps, unaligned reserved bytes, duplicate usable entries,
     * disabled entries, RAM at/above the non-PAE boundary. */
    PmmRange map[]={ {0,ram,1,1},{0,ram,1,1},{0,0x100000,2,1},
        {32*MIB+1,4094,4,1},{128*MIB,16*MIB,2,1},{0,ram,2,0},
        {UINT64_C(0x100000000),2*MIB,1,1} };
    unsigned count=ram>128*MIB ? 7 : 6;
    if (count==6) map[4]=(PmmRange){20*MIB,4096,2,1};
    PmmReservation reserved[]={ {MIB,3*MIB} };
    size_t bytes=pmm_metadata_bytes(map,count); assert(bytes>0);
    void *meta=malloc(bytes); assert(meta);
    uint32_t location;
    assert(!pmm_metadata_region(map,count,reserved,1,bytes,&location));
    assert((uint64_t)location+bytes<=ram && location>=4*MIB);
    Pmm p={0}; assert(!pmm_init(&p,map,count,reserved,1,meta,bytes));
    uint32_t free_before=p.free_pages; assert(free_before>0);
    if (ram>=UINT64_C(0x100000000)) assert(p.page_count==1048576);
    uint32_t usable=ram/MIB-(ram>128*MIB ? 17 : 1);
    if (ram>128*MIB) assert(p.firmware_usable==(uint64_t)usable*MIB-4094+2*MIB);
    PmmAllocation low={0},high={0};
    assert(!pmm_allocate(&p,8192,65536,0xffffff,65536,false,&low));
    assert(low.address>=4*MIB && low.address<16*MIB && !(low.address&65535));
    assert(!pmm_allocate(&p,16*MIB,4096,UINT32_MAX,0,true,&high));
    assert((uint64_t)high.address+high.bytes<=ram);
    if (ram>128*MIB) assert(high.address>64*MIB);
    PmmAllocation stale=low; assert(!pmm_release(&p,&low));
    assert(pmm_release(&p,&stale)==-22);
    low=(PmmAllocation){0}; assert(!pmm_allocate(&p,8192,65536,0xffffff,65536,false,&low));
    assert(pmm_release(&p,&stale)==-22 && low.cookie!=stale.cookie);
    assert(pmm_allocate(&p,1,4096,UINT32_MAX,0,false,&low)==-16);
    assert(!pmm_release(&p,&low) && !pmm_release(&p,&high));
    assert(p.free_pages==free_before && !p.allocation_count);
    free(meta);
}
static void exhaustive(void) {
    PmmRange map[]={{0,32*4096,1,1},{11*4096+1,1,2,1}};
    PmmReservation reserve[]={{0,4096},{4*4096,2*4096}};
    size_t bytes=pmm_metadata_bytes(map,2);void *meta=malloc(bytes);assert(meta);
    Pmm p={0};assert(!pmm_init(&p,map,2,reserve,2,meta,bytes));
    uint32_t before=p.free_pages;PmmAllocation blocks[32]={0};unsigned n=0;
    while(!pmm_allocate(&p,4096,4096,UINT32_MAX,0,false,&blocks[n])) {
        assert(blocks[n].address!=11*4096 && blocks[n].address!=4*4096 && blocks[n].address!=5*4096);
        assert(++n<32);
    }
    assert(n==before && !p.free_pages);
    for(unsigned i=0;i<n;i++)assert(!pmm_release(&p,&blocks[i]));
    assert(p.free_pages==before);free(meta);
    PmmRange overflow[]={{UINT64_MAX-8,16,1,1}};
    assert(!pmm_metadata_bytes(overflow,1));
}
int main(void) {
    test_map(64*MIB);test_map(256*MIB);test_map(UINT64_C(0x100000000));exhaustive();
    puts("PASS: E820 64 MiB/256 MiB/4 GiB, holes/reservations/overlaps, DMA masks/alignment, stale handles and exhaustion");
}

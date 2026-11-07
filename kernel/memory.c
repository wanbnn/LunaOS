#include "memory.h"
#include "driver.h"
#include "boot.h"
#include "heap.h"
static Pmm physical;
static uint32_t metadata_size;
static uint64_t initially_free;
extern const uint8_t __bss_end[], __bss_low_end[], __bss_high_start[];
void memory_init(void) {
    if (physical.ready) return;
    const PmmRange *map=(const void *)0x5004;
    uint32_t count=*(volatile uint32_t *)0x5000;
    PmmReservation reserved[8]; unsigned n=0;
    /* Boot buffers, firmware/VGA aperture and the complete linked kernel.
     * Keep the bootstrap/interrupt stack until stacks are relocated. */
    reserved[n++]=(PmmReservation){0,(uintptr_t)__bss_low_end};
    reserved[n++]=(PmmReservation){(uintptr_t)__bss_high_start,
        (uintptr_t)__bss_end-(uintptr_t)__bss_high_start};
    reserved[n++]=(PmmReservation){0x3d00000,0x300000};
    /* Direct physical pointers must remain valid in every task's CR3.
     * These virtual windows currently replace the identity mappings. */
    reserved[n++]=(PmmReservation){0x3c000000,0x1000000};
    reserved[n++]=(PmmReservation){0x40000000,0x400000};
    if (boot_is_uefi()) {
        const UefiBootInfo *info=LUNA_UEFI_INFO;
        reserved[n++]=(PmmReservation){info->ram_image,info->ram_bytes};
    }
    const uint8_t *vbe=(const void *)0x6000;
    uint32_t framebuffer,pitch,height;
    memcpy(&framebuffer,vbe+40,4);
    pitch=(uint32_t)vbe[16]|(uint32_t)vbe[17]<<8;
    height=(uint32_t)vbe[20]|(uint32_t)vbe[21]<<8;
    if (framebuffer && pitch && height)
        reserved[n++]=(PmmReservation){framebuffer,(uint64_t)pitch*height};
    metadata_size=pmm_metadata_bytes(map,count);
    uint32_t address=0;
    int rc=metadata_size ? pmm_metadata_region(map,count,reserved,n,metadata_size,&address) : DRV_EINVAL;
    if (!rc) {
        reserved[n++]=(PmmReservation){address,((uint64_t)metadata_size+4095)&~UINT64_C(4095)};
        rc=pmm_init(&physical,map,count,reserved,n,(void *)(uintptr_t)address,metadata_size);
    }
    if (rc) {
        serial_write("Physical memory: invalid map or metadata reservation failed\n");
        return; /* Never guess that unreported memory is safe. */
    }
    initially_free=(uint64_t)physical.free_pages*PMM_PAGE;
    memory_mb=(uint32_t)(physical.firmware_usable>>20);
    serial_write("Physical memory: E820-backed demand allocation / no fixed DMA or heap pool\n");
}
int memory_allocate(uint32_t bytes,uint32_t align,uint32_t mask,uint32_t boundary,
                      bool high,bool clear,PmmAllocation *out) {
    uint32_t flags=drv_irq_save();
    int rc=pmm_allocate(&physical,bytes,align,mask,boundary,high,out);
    drv_irq_restore(flags);
    if (!rc && clear) memset((void *)(uintptr_t)out->address,0,bytes);
    return rc;
}
int memory_release(const PmmAllocation *p) {
    uint32_t flags=drv_irq_save(); int rc=pmm_release(&physical,p); drv_irq_restore(flags); return rc;
}
int memory_lookup(uint32_t address,PmmAllocation *out) {
    uint32_t flags=drv_irq_save(); int rc=pmm_lookup(&physical,address,out); drv_irq_restore(flags); return rc;
}
void memory_stats(MemoryStats *out) {
    if (!out) return;
    uint32_t flags=drv_irq_save();
    uint64_t available=(uint64_t)physical.free_pages*PMM_PAGE;
    *out=(MemoryStats){.ready=physical.ready,.firmware_usable=physical.firmware_usable,
        .addressable_usable=physical.addressable_usable,.free_bytes=available,
        .above_4g_usable=physical.above_4g_usable,
        .allocated_bytes=initially_free-available,
        .reserved_bytes=physical.addressable_usable-initially_free,
        .allocations=physical.allocation_count,.metadata_bytes=metadata_size};
    drv_irq_restore(flags);
}
static void value(char *out,size_t cap,const char *label,uint64_t bytes) {
    char text[24]; append(out,label,cap);
    uint64_t kib=bytes/1024; unsigned count=0;
    do { text[count++]=(char)('0'+kib%10); kib/=10; } while (kib);
    for (unsigned i=0; i<count/2; i++) { char c=text[i]; text[i]=text[count-1-i]; text[count-1-i]=c; }
    text[count]=0; append(out,text,cap); append(out," KiB\n",cap);
}
void memory_status(char *out,size_t cap) {
    if (!out || !cap) return;
    MemoryStats s; memory_stats(&s); out[0]=0;
    if (!s.ready) { append(out,"Physical memory manager unavailable: invalid firmware map\n",cap); return; }
    append(out,"Physical memory / firmware map / demand allocation\n",cap);
    value(out,cap,"Firmware usable RAM: ",s.firmware_usable);
    value(out,cap,"Addressable RAM (i386 non-PAE): ",s.addressable_usable);
    value(out,cap,"Reserved (kernel/boot/mappings/metadata): ",s.reserved_bytes);
    value(out,cap,"Allocated pages (heap + DMA): ",s.allocated_bytes);
    value(out,cap,"Free pages (shared by heap and DMA): ",s.free_bytes);
    value(out,cap,"Allocator metadata: ",s.metadata_bytes);
    append(out,"Heap and DMA grow on demand; no 64 MiB RAM cap.\n",cap);
    if (s.above_4g_usable)
        append(out,"RAM above the 32-bit physical address space requires PAE or x86-64.\n",cap);
}

int memory_test(uint32_t mib,char *out,size_t cap) {
    if (!out || !cap || !mib || mib>4095) return DRV_EINVAL;
    out[0]=0;
    /* Exercise aggregate capacity without demanding one huge contiguous DMA
     * run. Firmware reservations legitimately fragment physical memory. */
    void *heaps[64]={0}; uint32_t lengths[64]={0}; DrvDma dmas[64]={0};
    uint32_t remaining=mib*1024*1024;
    uint32_t chunk=(((uint64_t)remaining+127)/128+PMM_PAGE-1)&~(uint64_t)(PMM_PAGE-1);
    MemoryStats before,after; memory_stats(&before);
    int rc=0;
    for (unsigned i=0; i<64 && remaining && !rc; i++) {
        uint32_t bytes=remaining<chunk ? remaining : chunk;
        heaps[i]=heap_allocate(bytes); lengths[i]=bytes;
        if (!heaps[i]) { rc=DRV_ENOMEM; break; }
        remaining-=bytes;
        if (!remaining) break;
        bytes=remaining<chunk ? remaining : chunk;
        rc=drv_dma_alloc(bytes,4096,UINT32_MAX,0,&dmas[i]);
        if (!rc) remaining-=bytes;
    }
    if (!rc && remaining) rc=DRV_ENOMEM;
    if (!rc) {
        for (unsigned block=0; block<64; block++) {
            if (heaps[block]) for (uint32_t i=0; i<lengths[block]/4; i+=1024)
                ((uint32_t *)heaps[block])[i]=i^block^0xa5b63d29;
            if (dmas[block].cookie) for (uint32_t i=0; i<dmas[block].size/4; i+=1024)
                ((uint32_t *)dmas[block].cpu)[i]=i^block^0x13579bdf;
        }
        for (unsigned block=0; block<64; block++) {
            if (heaps[block]) for (uint32_t i=0; i<lengths[block]/4; i+=1024)
                if (((volatile uint32_t *)heaps[block])[i]!=(i^block^0xa5b63d29)) rc=DRV_EIO;
            if (dmas[block].cookie) for (uint32_t i=0; i<dmas[block].size/4; i+=1024)
                if (((volatile uint32_t *)dmas[block].cpu)[i]!=(i^block^0x13579bdf)) rc=DRV_EIO;
        }
        char text[24]; append(out,"Heap address: ",cap); hex(text,(uintptr_t)heaps[0]); append(out,text,cap);
        append(out,"\nDMA address: ",cap); hex(text,dmas[0].bus); append(out,text,cap); append(out,"\n",cap);
    }
    for (unsigned i=0; i<64; i++) {
        if (heaps[i]) { int r=heap_release(heaps[i]); if (!rc) rc=r; }
        if (dmas[i].cookie) { int r=drv_dma_free(&dmas[i]); if (!rc) rc=r; }
    }
    memory_stats(&after);
    if (!rc && before.free_bytes!=after.free_bytes) rc=DRV_EIO;
    if (!rc) {
        char text[24]; number(text,mib); append(out,"MEMORY PASS: ",cap); append(out,text,cap);
        append(out," MiB heap + DMA / per-page patterns / full release\n",cap);
    } else append(out,"Memory allocation/test failed; completed allocations released.\n",cap);
    return rc;
}

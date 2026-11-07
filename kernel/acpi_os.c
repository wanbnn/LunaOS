#include "driver.h"
#include "boot.h"
#include "memory.h"
#include <uacpi/kernel_api.h>

/* Single CPU, boot-time AML only. SCI/GPE ownership is deliberately compiled
 * out; native device drivers retain their interrupts and PCI ownership. */
static bool checksum(const uint8_t *p,unsigned len) {
    uint8_t sum=0;while(len--)sum+=*p++;return sum==0;
}
static bool rsdp_valid(uint32_t p) {
    const uint8_t *v=(const void *)(uintptr_t)p;
    if(memcmp(v,"RSD PTR ",8) || !checksum(v,20))return false;
    if(v[15]>=2) {
        uint32_t len;memcpy(&len,v+20,4);
        if(len<36 || len>4096 || !checksum(v,len))return false;
    }
    return true;
}
uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    if(boot_is_uefi()) {
        if(LUNA_UEFI_INFO->rsdp_high)return UACPI_STATUS_MAPPING_FAILED;
        uint32_t addr=LUNA_UEFI_INFO->rsdp_low;
        if(addr && rsdp_valid(addr)){*out=addr;return UACPI_STATUS_OK;}
    }
    uint16_t segment;memcpy(&segment,(const void *)0x40e,2);
    uint32_t ebda=(uint32_t)segment<<4;
    if(ebda>=0x80000 && ebda<=0x9fc00)
        for(uint32_t p=ebda;p<ebda+1024;p+=16)if(rsdp_valid(p)){*out=p;return UACPI_STATUS_OK;}
    for(uint32_t p=0xe0000;p<=0xffff0;p+=16)if(rsdp_valid(p)){*out=p;return UACPI_STATUS_OK;}
    return UACPI_STATUS_NOT_FOUND;
}
void *uacpi_kernel_map(uacpi_phys_addr p,uacpi_size len) {
    if(!len || p>UINT32_MAX || len>0x1000000 || p+len>0x100000000ull)return UACPI_MAP_FAILED;
    /* Keep RAM mappings coherent with the kernel's identity mapping. */
    const PmmRange *map=(const void *)0x5004;
    unsigned count=*(const uint32_t *)0x5000;bool cached=false;
    for(unsigned i=0;i<count && i<128;i++)
        if((map[i].type==1 || map[i].type==3 || map[i].type==4) &&
           p>=map[i].base && p+len<=map[i].base+map[i].length){cached=true;break;}
    void *out=0;
    int rc=cached?task_physical_map_cached((uint32_t)p,len,&out):task_mmio_map((uint32_t)p,len,&out);
    return rc?UACPI_MAP_FAILED:out;
}
void uacpi_kernel_unmap(void *p,uacpi_size len){(void)task_mmio_unmap(p,len);}
void uacpi_kernel_log(uacpi_log_level level,const char *s) {
    (void)level;serial_write("ACPI: ");serial_write(s);
}
void *uacpi_kernel_alloc(uacpi_size size){return drv_alloc(size);}
void uacpi_kernel_free(void *p){if(p)drv_free(p);}
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void){return (uint64_t)drv_now_ms()*1000000u;}
void uacpi_kernel_stall(uacpi_u8 us){if(drv_udelay(us))drv_sleep_ms(1);}
void uacpi_kernel_sleep(uacpi_u64 ms){if(ms>3000)ms=3000;drv_sleep_ms((uint32_t)ms);}
uacpi_thread_id uacpi_kernel_get_thread_id(void){return (void *)(uintptr_t)1;}
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void){return drv_irq_save();}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state f){drv_irq_restore(f);}
uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 units) {
    uint32_t *p=drv_alloc(sizeof(*p));if(p)*p=units;return p;
}
void uacpi_kernel_free_semaphore(uacpi_handle p){if(p)drv_free(p);}
uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle p,uacpi_u16 timeout) {
    if(!p)return UACPI_STATUS_INVALID_ARGUMENT;
    uint32_t start=drv_now_ms();
    do {
        uint32_t flags=drv_irq_save();
        if(*(uint32_t *)p){--*(uint32_t *)p;drv_irq_restore(flags);return UACPI_STATUS_OK;}
        drv_irq_restore(flags);
        /* Infinite AML waits cannot be serviced by another boot-time thread. */
        if(!timeout || (uint32_t)(drv_now_ms()-start)>=(timeout==0xffff?3000:timeout))break;
        __asm__ volatile("pause");
    } while(true);
    return UACPI_STATUS_TIMEOUT;
}
void uacpi_kernel_signal_semaphore(uacpi_handle p) {
    uint32_t flags=drv_irq_save();if(p && *(uint32_t *)p<UINT32_MAX)++*(uint32_t *)p;drv_irq_restore(flags);
}
uacpi_handle uacpi_kernel_create_spinlock(void){return drv_alloc(1);}
void uacpi_kernel_free_spinlock(uacpi_handle p){if(p)drv_free(p);}
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle p){(void)p;return drv_irq_save();}
void uacpi_kernel_unlock_spinlock(uacpi_handle p,uacpi_cpu_flags f){(void)p;drv_irq_restore(f);}
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *r) {
    (void)r;serial_write("ACPI: firmware breakpoint/fatal request rejected\n");return UACPI_STATUS_UNIMPLEMENTED;
}
typedef struct {uint32_t base,len;} IoRange;
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base,uacpi_size len,uacpi_handle *out) {
    if(!len || base>65535 || base+len>65536)return UACPI_STATUS_INVALID_ARGUMENT;
    IoRange *io=drv_alloc(sizeof(*io));if(!io)return UACPI_STATUS_OUT_OF_MEMORY;
    *io=(IoRange){base,len};*out=io;return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle p){if(p)drv_free(p);}
#define IO_READ(bits,fn) \
uacpi_status uacpi_kernel_io_read##bits(uacpi_handle p,uacpi_size off,uacpi_u##bits *v) { \
    IoRange *io=p;if(!io || off>io->len || bits/8>io->len-off)return UACPI_STATUS_INVALID_ARGUMENT; \
    *v=fn(io->base+off);return UACPI_STATUS_OK;}
#define IO_WRITE(bits,fn) \
uacpi_status uacpi_kernel_io_write##bits(uacpi_handle p,uacpi_size off,uacpi_u##bits v) { \
    IoRange *io=p;if(!io || off>io->len || bits/8>io->len-off)return UACPI_STATUS_INVALID_ARGUMENT; \
    fn(io->base+off,v);return UACPI_STATUS_OK;}
IO_READ(8,inb) IO_READ(16,inw) IO_READ(32,inl)
IO_WRITE(8,outb) IO_WRITE(16,outw) IO_WRITE(32,outl)
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a,uacpi_handle *out) {
    if(a.segment || a.device>=32 || a.function>=8)return UACPI_STATUS_NOT_FOUND;
    uint32_t *p=drv_alloc(4);if(!p)return UACPI_STATUS_OUT_OF_MEMORY;
    *p=(a.bus<<16)|(a.device<<8)|a.function;*out=p;return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle p){if(p)drv_free(p);}
static uacpi_status config_read(void *p,unsigned off,unsigned width,uint32_t *v) {
    if(!p || off+width>256 || off%width)return UACPI_STATUS_INVALID_ARGUMENT;
    uint32_t a=*(uint32_t *)p;
    *v=pci_read(a>>16,a>>8,a,off)>>((off&3u)*8);
    if(width<4)*v&=(1u<<(width*8))-1;
    return UACPI_STATUS_OK;
}
static uacpi_status config_write(void *p,unsigned off,unsigned width,uint32_t v) {
    uint32_t old;uacpi_status rc=config_read(p,off&~3u,4,&old);if(rc)return rc;
    if(off+width>256 || off%width)return UACPI_STATUS_INVALID_ARGUMENT;
    uint32_t a=*(uint32_t *)p,shift=(off&3u)*8,mask=width==4?UINT32_MAX:(1u<<(width*8))-1;
    pci_write(a>>16,a>>8,a,off,(old&~(mask<<shift))|((v&mask)<<shift));return UACPI_STATUS_OK;
}
#define PCI_READ(bits) \
uacpi_status uacpi_kernel_pci_read##bits(uacpi_handle p,uacpi_size off,uacpi_u##bits *v) { \
    uint32_t x=0;uacpi_status rc=config_read(p,off,bits/8,&x);*v=x;return rc;}
#define PCI_WRITE(bits) \
uacpi_status uacpi_kernel_pci_write##bits(uacpi_handle p,uacpi_size off,uacpi_u##bits v) { \
    return config_write(p,off,bits/8,v);}
PCI_READ(8) PCI_READ(16) PCI_READ(32)
PCI_WRITE(8) PCI_WRITE(16) PCI_WRITE(32)
/* Reduced hardware builds do not request SCI/GPE interrupts. */
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,uacpi_interrupt_handler fn,uacpi_handle ctx,uacpi_handle *out) {
    (void)irq;(void)fn;(void)ctx;(void)out;return UACPI_STATUS_UNIMPLEMENTED;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn,uacpi_handle p) {
    (void)fn;(void)p;return UACPI_STATUS_UNIMPLEMENTED;
}
/* Dedicated boot-time queue: handlers may free or requeue their own item.
 * Never touch an item after invoking its handler (the uACPI API contract). */
typedef struct AcpiWork {struct AcpiWork *next;uacpi_work_handler fn;uacpi_handle arg;} AcpiWork;
static AcpiWork *work_head,*work_tail;
uacpi_handle uacpi_kernel_create_work_item(void){return drv_alloc(sizeof(AcpiWork));}
void uacpi_kernel_free_work_item(uacpi_handle p){if(p)drv_free(p);}
void uacpi_kernel_schedule_work(uacpi_work_type type,uacpi_handle p,uacpi_work_handler fn,uacpi_handle ctx) {
    (void)type;AcpiWork *w=p;uint32_t flags=drv_irq_save();
    w->fn=fn;w->arg=ctx;w->next=0;
    if(work_tail)work_tail->next=w;else work_head=w;
    work_tail=w;drv_irq_restore(flags);
}
uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    for(unsigned budget=0;budget<1024;budget++) {
        uint32_t flags=drv_irq_save();AcpiWork *w=work_head;
        if(!w){drv_irq_restore(flags);return UACPI_STATUS_OK;}
        work_head=w->next;if(!work_head)work_tail=0;
        uacpi_work_handler fn=w->fn;void *arg=w->arg;
        drv_irq_restore(flags);fn(arg);
    }
    return UACPI_STATUS_TIMEOUT;
}

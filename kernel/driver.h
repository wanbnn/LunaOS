#ifndef LUNA_DRIVER_H
#define LUNA_DRIVER_H
#include "luna.h"
/* Native driver API v1. Kernel-only, uniprocessor x86; not a Linux ABI. */
enum {
    DRV_OK = 0,
    DRV_ENOENT = -2,
    DRV_EIO = -5,
    DRV_ENOMEM = -12,
    DRV_EBUSY = -16,
    DRV_EINVAL = -22,
    DRV_ENOSPC = -28,
    DRV_EDEADLK = -35,
    DRV_ENOTSUP = -95,
    DRV_ETIMEDOUT = -110,
    DRV_EALREADY = -114
};
uint32_t drv_irq_save(void);
void drv_irq_restore(uint32_t);
bool drv_irqs_enabled(void);
bool drv_in_irq(void);
typedef struct {
    bool held;
} DrvLock;
int drv_lock(DrvLock *, uint32_t *flags);
void drv_unlock(DrvLock *, uint32_t flags);
void drv_memory_barrier(void);
uint32_t drv_now_ms(void);
uint32_t drv_cycles_per_us(void);
int drv_udelay(unsigned us);
int drv_sleep_ms(uint32_t ms);
typedef struct {
    uint32_t count;
} DrvCompletion;
void drv_complete(DrvCompletion *);
int drv_wait(DrvCompletion *, uint32_t timeout_ms);

typedef void (*DrvCallback)(void *);
typedef struct DrvWork {
    DrvCallback fn;
    void *arg;
    struct DrvWork *next;
    bool queued, running;
} DrvWork;
void drv_work_init(DrvWork *, DrvCallback, void *);
int drv_work_queue(DrvWork *);
int drv_work_cancel(DrvWork *);
typedef struct {
    DrvWork work;
    uint32_t deadline;
    bool armed;
} DrvTimer;
void drv_timer_init(DrvTimer *, DrvCallback, void *);
int drv_timer_start(DrvTimer *, uint32_t delay_ms);
int drv_timer_cancel(DrvTimer *);
void driver_services_init(void);
void driver_services_poll(unsigned budget);
bool drv_work_pending(void);

/* CPU and device addresses are intentionally separate. x86 PCI, no IOMMU. */
typedef struct {
    void *cpu;
    uint32_t bus, size, cookie;
} DrvDma;
int drv_dma_alloc(size_t size, size_t align, uint32_t mask, size_t boundary, DrvDma *);
int drv_dma_free(DrvDma *);
void *drv_alloc(size_t);
int drv_free(void *);
size_t drv_dma_available(void);

typedef struct {
    uint8_t bus, slot, function, revision, irq, pin;
    uint16_t vendor, device;
    uint32_t subsystem, class_code, cookie;
} DrvPci;
typedef struct {
    uint32_t address, size;
    bool io, prefetch;
} DrvBar;
int drv_pci_find(uint16_t vendor, uint16_t device, unsigned index, DrvPci *);
int drv_pci_claim(DrvPci *);
int drv_pci_config_read(DrvPci *, unsigned offset, unsigned width, uint32_t *);
int drv_pci_config_write(DrvPci *, unsigned offset, unsigned width, uint32_t);
int drv_pci_release(DrvPci *);
int drv_pci_bar(DrvPci *, unsigned index, DrvBar *);
int drv_pci_enable(DrvPci *, bool io, bool memory);
int drv_pci_master(DrvPci *, bool enabled);
int drv_pci_msi_enable(DrvPci *); /* Explicit ENOTSUP until APIC/MSI exists. */
typedef struct {
    volatile uint8_t *base;
    uint32_t size, cookie;
} DrvMmio;
int drv_pci_map(DrvPci *, unsigned bar, DrvMmio *);
int drv_mmio_unmap(DrvMmio *);
int drv_mmio_read(DrvMmio *, uint32_t offset, unsigned width, uint32_t *);
int drv_mmio_write(DrvMmio *, uint32_t offset, unsigned width, uint32_t);
typedef bool (*DrvIrqHandler)(void *);
typedef struct {
    uint32_t cookie;
} DrvIrq;
int drv_irq_request(DrvPci *, DrvIrqHandler, void *, DrvIrq *);
int drv_irq_free(DrvIrq *);
void driver_irq_dispatch(unsigned line);
/* Paging backend, shared supervisor-only mappings, internal to driver services. */
int task_mmio_map(uint32_t physical, uint32_t bytes, void **virtual_address);
int task_physical_map_cached(uint32_t physical, uint32_t bytes, void **virtual_address);
int drv_platform_map(uint32_t physical, uint32_t bytes, DrvMmio *out);
int task_mmio_unmap(void *virtual_address, uint32_t bytes);

#define DRV_PACKET_CAP 4096
#define DRV_PACKET_QUEUE_MAX 64
typedef struct DrvPacket {
    struct DrvPacket *next;
    uint8_t *storage;
    uint32_t capacity, offset, length, refs;
    bool queued;
    DrvDma allocation;
} DrvPacket;
typedef struct {
    DrvPacket *head, *tail;
    unsigned count;
} DrvPacketQueue;
DrvPacket *drv_packet_alloc(unsigned payload, unsigned headroom);
int drv_packet_ref(DrvPacket *);
int drv_packet_release(DrvPacket *);
void *drv_packet_put(DrvPacket *, unsigned);
void *drv_packet_push(DrvPacket *, unsigned);
void *drv_packet_pull(DrvPacket *, unsigned);
int drv_packet_enqueue(DrvPacketQueue *, DrvPacket *);
DrvPacket *drv_packet_dequeue(DrvPacketQueue *);
void drv_packet_purge(DrvPacketQueue *);

typedef struct {
    const uint8_t *data;
    uint32_t size, checksum;
    DrvDma allocation;
} DrvFirmware;
int drv_firmware_request(const char *name, DrvFirmware *);
int drv_firmware_release(DrvFirmware *);
void driver_services_status(char *, size_t);
int driver_services_test(char *, size_t);
#endif

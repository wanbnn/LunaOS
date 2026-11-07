#ifndef LUNA_STORAGE_H
#define LUNA_STORAGE_H
#include "install.h"
#include "driver.h"

/* Native block layer v1: fixed-size registry, 512-byte logical sectors,
 * bounded synchronous DMA transfers, no dynamic allocation in the registry.
 * Indices 0..3 are legacy ATA, 4 is the UEFI RAM/optical boot source. */
#define STORAGE_NATIVE_BASE 5u
#define STORAGE_NATIVE_SLOTS 8u
#define STORAGE_DEVICE_COUNT (STORAGE_NATIVE_BASE + STORAGE_NATIVE_SLOTS)
#define STORAGE_MAX_TRANSFER 64u
#define STORAGE_CONTROLLER_MAX 24u
enum {
    STORAGE_ATA=1, STORAGE_AHCI, STORAGE_NVME, STORAGE_USB,
    STORAGE_EMMC, STORAGE_RAID
};
typedef int (*StorageTransfer)(void *ctx, uint64_t lba, unsigned sectors,
                               void *data, bool write);
typedef int (*StorageFlush)(void *ctx);
typedef struct {
    void *ctx;
    StorageTransfer transfer;
    StorageFlush flush;
    uint64_t sectors;
    uint32_t logical_bytes;
    uint8_t transport;
    bool removable, readonly, installable;
    char model[41];
} StorageDevice;
typedef struct {
    uint8_t bus, slot, function, transport;
    uint16_t vendor, device;
    bool initialized;
    char reason[70];
} StorageController;

void storage_reset(void);
int storage_register(const StorageDevice *dev);
const DiskInfo *storage_disk_info(unsigned index);
int storage_transfer(unsigned index, uint32_t lba, unsigned sectors,
                     void *buffer, bool write);
int storage_flush(unsigned index);
unsigned storage_device_count(void);
const char *storage_disk_type(unsigned index);
void storage_pci_init(void);
void storage_status(char *out, size_t cap);
int storage_verify_read(unsigned index, char *out, size_t cap);
unsigned storage_controller_count(void);
const StorageController *storage_controller(unsigned index);

/* Polled controller backends; no bus mastering before safe claim/validation. */
int storage_ahci_probe(uint8_t bus, uint8_t slot, uint8_t function);
int storage_nvme_probe(uint8_t bus, uint8_t slot, uint8_t function);
#endif

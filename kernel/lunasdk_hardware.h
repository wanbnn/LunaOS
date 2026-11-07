#ifndef LUNASDK_HARDWARE_H
#define LUNASDK_HARDWARE_H
#include <stdint.h>

/* LunaSDK hardware ABI v2: real PCI inventory / read-only config space.
 * No raw writes, MMIO, DMA or IRQ are exposed to scripting runtimes. */
typedef struct {
    uint8_t bus, slot, function, revision, irq, pin;
    uint16_t vendor, device;
    uint32_t class_code, subsystem;
} LunaPciInfo;

/* Discover devices on the actual PCI bus, not a hardcoded/simulated list. */
int lunasdk_pci_get(unsigned index, LunaPciInfo *out);
int lunasdk_pci_count(unsigned *count);
int lunasdk_pci_read(unsigned bus, unsigned slot, unsigned function,
                     unsigned offset, unsigned width, uint32_t *value);
#endif

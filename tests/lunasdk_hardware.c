#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../kernel/lunasdk_hardware.h"

/* Synthetic PCI read backend for deterministic *host* tests. The LunaOS
 * runtime implementation uses its real PCI CF8/CFC accessor instead. */
static unsigned writes;
uint32_t pci_read(uint8_t b, uint8_t s, uint8_t f, uint8_t reg) {
    if (b != 0 || s > 1 || f > 1) return 0xffffffffu;
    if (s == 0 && f == 0) {
        if (reg == 0) return 0x12348086u;
        if (reg == 0x0c) return 0x00800000u; /* multifunction */
        if (reg == 8) return 0x01060103u;
        if (reg == 0x2c) return 0xaabbccdd;
        if (reg == 0x3c) return 0x0000010bu;
    }
    if (s == 0 && f == 1) {
        if (reg == 0) return 0xbeef10ecu;
        if (reg == 8) return 0x02800001u;
    }
    if (s == 1 && f == 0) {
        if (reg == 0) return 0x22221111u;
        if (reg == 0x0c) return 0;
    }
    return 0;
}
int main(void) {
    unsigned count = 0;
    assert(!lunasdk_pci_count(&count) && count == 3);
    LunaPciInfo d = {0};
    assert(!lunasdk_pci_get(0, &d));
    assert(d.vendor == 0x8086 && d.device == 0x1234 && d.bus == 0);
    assert(d.slot == 0 && d.function == 0 && d.class_code == 0x010601);
    assert(d.irq == 11 && d.pin == 1);
    assert(!lunasdk_pci_get(1, &d) && d.vendor == 0x10ec && d.function == 1);
    assert(!lunasdk_pci_get(2, &d) && d.vendor == 0x1111 && d.slot == 1);
    assert(lunasdk_pci_get(3, &d) == -2);
    uint32_t value;
    assert(!lunasdk_pci_read(0, 0, 0, 0, 2, &value) && value == 0x8086);
    assert(!lunasdk_pci_read(0, 0, 0, 2, 2, &value) && value == 0x1234);
    assert(!lunasdk_pci_read(0, 0, 0, 0x3c, 1, &value) && value == 11);
    assert(lunasdk_pci_read(0, 0, 0, 3, 2, &value) == -22);
    assert(lunasdk_pci_read(0, 0, 0, 256, 4, &value) == -22);
    assert(lunasdk_pci_read(0, 31, 0, 0, 4, &value) == -2);
    assert(!writes);
    puts("PASS: LunaSDK physical PCI discovery and read-only config API (mock host bus)");
}

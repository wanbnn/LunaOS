#include "lunasdk_hardware.h"
#include "luna.h"

/* PCI access delegates to drivers/pci.c, which serializes CF8/CFC via IRQ-save.
 * The SDK never claims devices or changes their configuration registers. */
static bool present(uint32_t id) {
    uint16_t vendor = (uint16_t)id;
    return vendor != 0xffff && vendor != 0;
}
int lunasdk_pci_get(unsigned index, LunaPciInfo *out) {
    if (!out) return -22;
    for (unsigned b = 0; b < 256; b++)
        for (unsigned s = 0; s < 32; s++) {
            uint32_t first = pci_read(b, s, 0, 0);
            if (!present(first)) continue;
            unsigned fn_count = (pci_read(b, s, 0, 0x0c) & 0x00800000u) ? 8 : 1;
            for (unsigned f = 0; f < fn_count; f++) {
                uint32_t id = f ? pci_read(b, s, f, 0) : first;
                if (!present(id)) continue;
                if (index--) continue;
                uint32_t cls = pci_read(b, s, f, 8);
                uint32_t irq = pci_read(b, s, f, 0x3c);
                *out = (LunaPciInfo){
                    .bus = (uint8_t)b, .slot = (uint8_t)s, .function = (uint8_t)f,
                    .vendor = (uint16_t)id, .device = (uint16_t)(id >> 16),
                    .revision = (uint8_t)cls, .class_code = cls >> 8,
                    .subsystem = pci_read(b, s, f, 0x2c),
                    .irq = (uint8_t)irq, .pin = (uint8_t)(irq >> 8)
                };
                return 0;
            }
        }
    return -2;
}
int lunasdk_pci_count(unsigned *count) {
    if (!count) return -22;
    unsigned n = 0;
    for (unsigned b = 0; b < 256; b++)
        for (unsigned s = 0; s < 32; s++) {
            uint32_t first = pci_read(b, s, 0, 0);
            if (!present(first)) continue;
            unsigned fn_count = (pci_read(b, s, 0, 0x0c) & 0x00800000u) ? 8 : 1;
            for (unsigned f = 0; f < fn_count; f++)
                if (present(f ? pci_read(b, s, f, 0) : first)) n++;
        }
    *count = n;
    return 0;
}
int lunasdk_pci_read(unsigned bus, unsigned slot, unsigned function,
                     unsigned offset, unsigned width, uint32_t *value) {
    if (!value || bus > 255 || slot > 31 || function > 7 || offset > 255 ||
        (width != 1 && width != 2 && width != 4) ||
        offset % width || width > 256 - offset)
        return -22;
    uint32_t first = pci_read(bus, slot, function, 0);
    if (!present(first)) return -2;
    uint32_t word = pci_read(bus, slot, function, offset & ~3u);
    uint32_t v = word >> ((offset & 3u) * 8);
    *value = width == 4 ? v : v & ((1u << (width * 8)) - 1u);
    return 0;
}

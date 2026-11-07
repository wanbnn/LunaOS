#include "driver.h"
#include "luna.h"
uint32_t pci_read(uint8_t b, uint8_t d, uint8_t f, uint8_t o) {
    uint32_t flags = drv_irq_save();
    outl(0xcf8, 0x80000000 | ((uint32_t)b << 16) | (d << 11) | (f << 8) | (o & 0xfc));
    uint32_t result = inl(0xcfc);
    drv_irq_restore(flags);
    return result;
}
void pci_write(uint8_t b, uint8_t d, uint8_t f, uint8_t o, uint32_t v) {
    uint32_t flags = drv_irq_save();
    outl(0xcf8, 0x80000000 | ((uint32_t)b << 16) | (d << 11) | (f << 8) | (o & 0xfc));
    outl(0xcfc, v);
    drv_irq_restore(flags);
}
void pci_list(char *out, size_t cap) {
    out[0] = 0;
    for (int b = 0; b < 256; b++)
        for (int d = 0; d < 32; d++)
            for (int f = 0; f < 8; f++) {
                uint32_t id = pci_read(b, d, f, 0);
                if ((id & 65535) == 65535)
                    continue;
                char tmp[16];
                number(tmp, b);
                append(out, tmp, cap);
                append(out, ":", cap);
                number(tmp, d);
                append(out, tmp, cap);
                append(out, ".", cap);
                number(tmp, f);
                append(out, tmp, cap);
                append(out, "  ", cap);
                hex(tmp, id);
                append(out, tmp, cap);
                append(out, "\n", cap);
            }
}

/* Mocks exercise failures and resource cleanup; no Wi-Fi hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../drivers/rtw8822be_diag.h"
#include "../kernel/driver.h"
static int unavailable, missing_fw, bad_mmio, map_fails, claims, maps, fwrefs;
void append(char *d, const char *s, size_t cap) {
    size_t n = strlen(d), m = strlen(s);
    if (n + m < cap) memcpy(d + n, s, m + 1);
}
void number(char *d, uint32_t v) { sprintf(d, "%u", v); }
void hex(char *d, uint32_t v) { sprintf(d, "%08x", v); }
int drv_pci_find(uint16_t v, uint16_t d, unsigned idx, DrvPci *out) {
    assert(v == 0x10ec && d == 0xb822 && idx == 0);
    if (unavailable) return DRV_ENOENT;
    *out = (DrvPci){.vendor = v, .device = d, .class_code = 0x028000};
    return 0;
}
int drv_pci_claim(DrvPci *p) {
    assert(!claims && !p->cookie);
    claims++; p->cookie = 1; return 0;
}
int drv_pci_config_read(DrvPci *p, unsigned o, unsigned w, uint32_t *out) {
    assert(p->cookie && o == 4 && w == 2);
    *out = 0; return 0;
}
int drv_pci_bar(DrvPci *p, unsigned idx, DrvBar *out) {
    assert(p->cookie && idx == 2);
    *out = (DrvBar){.address = 0xf0000000, .size = 0x1000}; return 0;
}
int drv_pci_enable(DrvPci *p, bool io, bool mem) {
    assert(p->cookie && !io && mem); return 0;
}
int drv_pci_map(DrvPci *p, unsigned bar, DrvMmio *out) {
    assert(p->cookie && bar == 2);
    if (map_fails) return DRV_EIO;
    out->cookie = 1; out->size = 0x1000; maps++; return 0;
}
int drv_mmio_read(DrvMmio *m, uint32_t off, unsigned w, uint32_t *out) {
    assert(m->cookie && off == 0xf0 && w == 4);
    *out = bad_mmio ? 0xffffffffu : 0x12345678u; return 0;
}
int drv_mmio_unmap(DrvMmio *m) {
    assert(m->cookie && maps == 1);
    maps--; m->cookie = 0; return 0;
}
int drv_pci_release(DrvPci *p) {
    assert(p->cookie && claims == 1 && maps == 0);
    claims--; p->cookie = 0; return 0;
}
int drv_firmware_request(const char *name, DrvFirmware *out) {
    assert(!strcmp(name, "rtw88/rtw8822b_fw.bin"));
    if (missing_fw) return DRV_ENOENT;
    static uint8_t bin[80];
    out->size = sizeof(bin); out->data = bin; fwrefs++; return 0;
}
int drv_firmware_release(DrvFirmware *fw) {
    assert(fw->data && fwrefs == 1);
    fw->data = 0; fwrefs--; return 0;
}
int main(void) {
    char s[512];
    assert(rtw8822be_diag(s, sizeof(s)) == 0);
    assert(strstr(s, "NOT initialized") && !claims && !maps && !fwrefs);
    missing_fw = 1;
    assert(rtw8822be_diag(s, sizeof(s)) == DRV_ENOENT);
    assert(!claims && !maps && !fwrefs);
    missing_fw = 0; bad_mmio = 1;
    assert(rtw8822be_diag(s, sizeof(s)) == DRV_EIO);
    assert(!claims && !maps);
    bad_mmio = 0; map_fails = 1;
    assert(rtw8822be_diag(s, sizeof(s)) == DRV_EIO);
    assert(!claims && !maps);
    map_fails = 0; unavailable = 1;
    assert(rtw8822be_diag(s, sizeof(s)) == DRV_ENOENT);
    assert(!claims && !maps);
    return 0;
}

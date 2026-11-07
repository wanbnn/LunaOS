#include <assert.h>
#include <string.h>
#include "../drivers/wifi_intel.h"
static bool present = true, wrong_class;
static int error;
int drv_pci_find(uint16_t vendor, uint16_t device, unsigned index, DrvPci *out) {
    assert(vendor == 0x8086);
    if (error) return error;
    if (!present || device != 0x51f0 || index > 1) return DRV_ENOENT;
    *out = (DrvPci){.vendor=vendor, .device=device, .subsystem=index ? 0x12348086 : 0x40908086,
        .class_code=wrong_class ? 0x010000 : 0x028000, .revision=1};
    return 0;
}
int main(void) {
    WifiIntelProbe p;
    assert(wifi_intel_probe(0, 0)==DRV_EINVAL);
    assert(!wifi_intel_probe(0, &p) && p.ax211_machine_match && p.pci.revision==1);
    assert(!wifi_intel_probe(1, &p) && !p.ax211_machine_match);
    assert(wifi_intel_probe(2, &p)==DRV_ENOENT && !p.pci.vendor);
    wrong_class=true;
    assert(wifi_intel_probe(0, &p)==DRV_ENOENT);
    wrong_class=false; present=false;
    assert(wifi_intel_probe(0, &p)==DRV_ENOENT);
    error=DRV_EIO;
    assert(wifi_intel_probe(0, &p)==DRV_EIO);
}

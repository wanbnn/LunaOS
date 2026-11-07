/* Host mocks validate resource handling; not a physical radio test. */
#include <assert.h>
#include <string.h>
#include "../drivers/wifi_port.h"
static unsigned found, claimed, released, requested, freed;
static int missing_fw, claim_error, wrong_class, missing_bar;
int drv_pci_find(uint16_t v, uint16_t d, unsigned idx, DrvPci *out) {
    if (idx || !wifi_port_match(v, d) || !found) return DRV_ENOENT;
    *out = (DrvPci){.vendor = v, .device = d,
                    .class_code = wrong_class ? 0x010000 : 0x028000,
                    .revision = 1, .subsystem = 0x804c103c};
    return 0;
}
int drv_pci_claim(DrvPci *pci) {
    if (claim_error) return DRV_EBUSY;
    assert(pci->vendor && !claimed);
    claimed++; pci->cookie = 1; return 0;
}
int drv_pci_bar(DrvPci *pci, unsigned idx, DrvBar *out) {
    assert(pci->cookie && idx < 6);
    if (missing_bar || idx != 2) return DRV_ENOENT;
    *out = (DrvBar){.address = 0xf0000000, .size = 0x2000};
    return 0;
}
int drv_pci_release(DrvPci *pci) {
    assert(pci->cookie);
    pci->cookie = 0; claimed--; released++; return 0;
}
int drv_firmware_request(const char *name, DrvFirmware *out) {
    assert(name && !out->data && !out->allocation.cookie);
    requested++;
    if (missing_fw) return DRV_ENOENT;
    out->data = (const uint8_t *)name;
    return 0;
}
int drv_firmware_release(DrvFirmware *fw) {
    assert(fw->data);
    freed++; fw->data = 0; return 0;
}
static void one(const WifiPortTarget *t, unsigned aux) {
    WifiPortProbe p;
    found = 1;
    missing_fw = claim_error = wrong_class = missing_bar = 0;
    requested = freed = released = 0;
    assert(wifi_port_preflight(t, 0, &p) == DRV_OK);
    assert(p.pci_present && p.mmio_ready && p.mmio_bar == 2);
    assert(p.firmware_ready && !p.driver_loaded && !p.pci.cookie);
    assert(requested == 1 + aux && freed == requested && released == 1);
    missing_fw = 1;
    assert(wifi_port_preflight(t, 0, &p) == DRV_OK);
    assert(!p.firmware_ready && p.firmware_result == DRV_ENOENT);
    assert(!claimed && released == 2);
    missing_fw = 0; missing_bar = 1;
    assert(wifi_port_preflight(t, 0, &p) == DRV_OK);
    assert(!p.mmio_ready && p.mmio_bar == 6 && !claimed);
    wrong_class = 1;
    assert(wifi_port_preflight(t, 0, &p) == DRV_ENOTSUP);
    assert(p.pci_present && !claimed);
    wrong_class = 0; claim_error = 1;
    assert(wifi_port_preflight(t, 0, &p) == DRV_EBUSY);
    assert(!claimed);
    claim_error = 0; found = 0;
    assert(wifi_port_preflight(t, 0, &p) == DRV_ENOENT);
    assert(!p.pci_present);
}
int main(void) {
    assert(wifi_port_target_count() == 7);
    assert(!wifi_port_target(7) && !wifi_port_match(0xffff, 0xffff));
    for (unsigned i = 0; i < wifi_port_target_count(); i++) {
        const WifiPortTarget *t = wifi_port_target(i);
        assert(t && wifi_port_match(t->vendor, t->device) == t);
        one(t, t->firmware_aux != 0);
    }
    WifiPortProbe p;
    assert(wifi_port_preflight(0, 0, &p) == DRV_EINVAL);
    assert(wifi_port_preflight(wifi_port_target(0), 0, 0) == DRV_EINVAL);
    return 0;
}

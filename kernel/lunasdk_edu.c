/* Native C driver example: QEMU EDU PCI device, vendor 1234 device 11e8.
 * Demonstrates real PCI ownership, BAR0 MMIO and SDK endpoint registration.
 * Not a mock. Only probes on machines where the device is actually present. */
#include "devkit.h"
#include "driver.h"

static DrvPci edu_pci;
static DrvMmio edu_mmio;
static int edu_read(void *context, char *out, size_t cap) {
    (void)context;
    uint32_t id = 0;
    int rc = drv_mmio_read(&edu_mmio, 0, 4, &id);
    if (rc) return rc;
    char value[16];
    hex(value, id);
    if (strlen(value) >= cap) return DRV_ENOSPC;
    strcpy(out, value);
    return 0;
}
void lunasdk_edu_init(void) {
    if (drv_pci_find(0x1234, 0x11e8, 0, &edu_pci))
        return; /* absent on most real PCs; do not invent a device */
    if (drv_pci_claim(&edu_pci))
        return;
    if (drv_pci_enable(&edu_pci, false, true))
        goto release;
    if (drv_pci_map(&edu_pci, 0, &edu_mmio))
        goto release;
    /* Hardware endpoint is read-only, because random MMIO writes can hang the OS. */
    if (ldev_native_register("qemu-edu", edu_read, 0, 0)) {
        drv_mmio_unmap(&edu_mmio);
        goto release;
    }
    serial_write("LunaSDK: QEMU EDU native PCI/MMIO endpoint ready\n");
    return;
release:
    drv_pci_release(&edu_pci);
}

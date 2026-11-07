#include "wifi_intel.h"
/* Linux v6.12 iwlwifi/pcie/drv.c: these CNVi controllers require RF/CSR
 * identification before selecting an iwl_cfg and firmware. Do not infer an
 * AX211 or a firmware filename from the controller PCI ID alone. */
static const uint16_t controllers[] = {0x51f0, 0x51f1, 0x54f0, 0x7a70, 0x7af0, 0x7f70};
int wifi_intel_probe(unsigned index, WifiIntelProbe *out) {
    if (!out) return DRV_EINVAL;
    memset(out, 0, sizeof(*out));
    for (unsigned i = 0; i < sizeof(controllers)/sizeof(controllers[0]); i++) {
        for (unsigned instance = 0;; instance++) {
            DrvPci pci = {0};
            int rc = drv_pci_find(0x8086, controllers[i], instance, &pci);
            if (rc == DRV_ENOENT) break;
            if (rc) return rc;
            if (pci.class_code >> 16 != 2) continue;
            if (index) { index--; continue; }
            out->pci = pci;
            /* Identified through Windows on the development machine. This is
             * a diagnostic label, not permission to choose a hardware config. */
            out->ax211_machine_match = pci.device == 0x51f0 &&
                pci.subsystem == 0x40908086;
            return DRV_OK;
        }
    }
    return DRV_ENOENT;
}
void wifi_intel_status(char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    for (unsigned i = 0;; i++) {
        WifiIntelProbe p;
        int rc = wifi_intel_probe(i, &p);
        if (rc == DRV_ENOENT) break;
        if (rc) { append(out, "Intel CNVi: PCI inventory error.\n", cap); break; }
        append(out, p.ax211_machine_match ? "Intel AX211" : "Intel CNVi (AX211 candidate)", cap);
        char value[24];
        append(out, " PCI=8086:", cap); hex(value, p.pci.device); append(out, value, cap);
        append(out, " subsystem=", cap); hex(value, p.pci.subsystem); append(out, value, cap);
        append(out, " revision=", cap); hex(value, p.pci.revision); append(out, value, cap);
        append(out, "\nIntel firmware parser + shared WPA2/IP core available. Gen3 DMA/firmware boot, NVM/regulatory and RF backend pending. Run wifidiag intel. Wi-Fi inactive.\n", cap);
    }
}

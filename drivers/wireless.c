#include "luna.h"
#include "wifi_port.h"
#include "rtw8822be_pcie.h"
/* Other devices remain discovery-only. */
typedef struct { uint16_t vendor, device; const char *name; } WirelessTarget;
static const WirelessTarget other_targets[] = {
    {0x10ec, 0xc822, "Realtek RTL8822CE"},
    {0x14c3, 0x7615, "MediaTek MT7615"},
    {0x14c3, 0x7663, "MediaTek MT7663"},
    {0x14c3, 0x7961, "MediaTek MT7921"},
    {0x14c3, 0x7922, "MediaTek MT7922"},
    {0x8086, 0x08b1, "Intel Wireless 7260"},
    {0x8086, 0x24fd, "Intel Wireless 8265"},
    {0x8086, 0x2723, "Intel AX200"},
    {0x14e4, 0x43ba, "Broadcom BCM43602"}
};
void wireless_status(char *out, size_t cap) {
    static bool scanned;
    static char report[2048];
    if (!out || !cap) return;
    if (!scanned) {
        /* Do not repeat PCI BAR sizing/firmware reads at every repaint. */
        scanned = true;
        report[0] = 0;
        unsigned devices = 0;
        if (rtw8822be_present()) {
            rtw8822be_status(report, sizeof(report));
            devices++;
        }
        for (unsigned i = 0; i < wifi_port_target_count(); i++) {
            const WifiPortTarget *t = wifi_port_target(i);
            if (t->vendor == 0x10ec && t->device == 0xb822 && rtw8822be_present())
                continue; /* Native owner: never BAR-size an active DMA device. */
            for (unsigned instance = 0; instance < 2; instance++) {
                WifiPortProbe p;
                int rc = wifi_port_preflight(t, instance, &p);
                if (!p.pci_present) break;
                devices++;
                append(report, t->name, sizeof(report));
                append(report, " [", sizeof(report));
                append(report, t->family_name, sizeof(report));
                append(report, "] subsystem=", sizeof(report));
                char n[24];
                hex(n, p.pci.subsystem);
                append(report, n, sizeof(report));
                append(report, " revision=", sizeof(report));
                hex(n, p.pci.revision);
                append(report, n, sizeof(report));
                if (rc) {
                    append(report, " PCI error=", sizeof(report));
                    hex(n, (uint32_t)-rc);
                    append(report, n, sizeof(report));
                } else {
                    append(report, p.mmio_ready ? " MMIO BAR ready" :
                           " MMIO BAR unavailable", sizeof(report));
                    if (p.firmware_ready) {
                        append(report, " firmware readable", sizeof(report));
                    } else {
                        append(report, " firmware missing/invalid: ", sizeof(report));
                        append(report, p.firmware_result ? t->firmware : t->firmware_aux,
                               sizeof(report));
                    }
                }
                append(report, "\nDriver NOT ported; Wi-Fi inactive.\n", sizeof(report));
            }
        }
        for (unsigned i = 0; i < sizeof(other_targets)/sizeof(other_targets[0]); i++) {
            DrvPci p = {0};
            if (!drv_pci_find(other_targets[i].vendor, other_targets[i].device, 0, &p)) {
                devices++;
                append(report, other_targets[i].name, sizeof(report));
                append(report, ": detected, driver pending.\n", sizeof(report));
            }
        }
        if (!devices)
            append(report, "Wi-Fi: no target PCIe device detected.\n", sizeof(report));
        append(report, "Bluetooth: USB/HCI stack pending. No wireless connection is active.",
               sizeof(report));
    }
    out[0] = 0;
    append(out, report, cap);
}

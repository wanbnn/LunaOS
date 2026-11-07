#include "luna.h"
/* Discovery only. Never enable PCI bus mastering before a real DMA driver exists. */
typedef struct {
    uint16_t vendor, device;
    const char *name;
} WirelessTarget;
static const WirelessTarget targets[] = {
    {0x10ec, 0xb723, "Realtek RTL8723BE"},   {0x10ec, 0x8179, "Realtek RTL8188EE"},
    {0x10ec, 0xd723, "Realtek RTL8723DE"},   {0x10ec, 0xc821, "Realtek RTL8821CE"},
    {0x10ec, 0xc822, "Realtek RTL8822CE"},   {0x14c3, 0x7612, "MediaTek MT7612"},
    {0x14c3, 0x7615, "MediaTek MT7615"},     {0x14c3, 0x7663, "MediaTek MT7663"},
    {0x14c3, 0x7961, "MediaTek MT7921"},     {0x14c3, 0x7922, "MediaTek MT7922"},
    {0x8086, 0x08b1, "Intel Wireless 7260"}, {0x8086, 0x24fd, "Intel Wireless 8265"},
    {0x8086, 0x2723, "Intel AX200"},         {0x168c, 0x0042, "Qualcomm QCA9377"},
    {0x14e4, 0x43ba, "Broadcom BCM43602"},
};
void wireless_status(char *out, size_t cap) {
    static bool scanned;
    static const WirelessTarget *detected;
    static uint32_t subsystem;
    if (!scanned) {
        scanned = true;
        for (int b = 0; b < 256; b++)
            for (int d = 0; d < 32; d++) {
                uint32_t id = pci_read(b, d, 0, 0);
                if ((id & 65535) == 65535)
                    continue;
                for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
                    if ((id & 65535) == targets[i].vendor && (id >> 16) == targets[i].device) {
                        detected = &targets[i];
                        subsystem = pci_read(b, d, 0, 0x2c);
                    }
                }
            }
    }
    out[0] = 0;
    if (!detected)
        append(out, "Wi-Fi: no target PCIe device detected.\n", cap);
    else {
        append(out, detected->name, cap);
        char n[16];
        append(out, " detected / subsystem ", cap);
        hex(n, subsystem);
        append(out, n, cap);
        append(out, "\nWi-Fi driver and firmware initialization pending.\n", cap);
    }
    append(out, "Bluetooth: USB controller / HCI stack pending.\nNo wireless connection is active.",
           cap);
}

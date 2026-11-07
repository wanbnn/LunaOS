#include "wifi_port.h"
/* Firmware names from upstream Linux sources. MT7612E is the PCIe variant.
 * QCA9377 firmware/API and board data need hardware revision verification. */
static const WifiPortTarget targets[] = {
    {0x10ec, 0xb723, "Realtek RTL8723BE", "rtlwifi", WIFI_RTLWIFI,
     "rtlwifi/rtl8723befw_36.bin", 0},
    {0x10ec, 0x8179, "Realtek RTL8188EE", "rtlwifi", WIFI_RTLWIFI,
     "rtlwifi/rtl8188efw.bin", 0},
    {0x10ec, 0xd723, "Realtek RTL8723DE", "rtw88", WIFI_RTW88,
     "rtw88/rtw8723d_fw.bin", 0},
    {0x10ec, 0xc821, "Realtek RTL8821CE", "rtw88", WIFI_RTW88,
     "rtw88/rtw8821c_fw.bin", 0},
    {0x10ec, 0xb821, "Realtek RTL8821CE (alt PCI ID)", "rtw88", WIFI_RTW88,
     "rtw88/rtw8821c_fw.bin", 0},
    {0x14c3, 0x7612, "MediaTek MT7612E", "mt76x2e", WIFI_MT76,
     "mt7662.bin", "mt7662_rom_patch.bin"},
    {0x168c, 0x0042, "Qualcomm QCA9377", "ath10k", WIFI_ATH10K,
     "ath10k/QCA9377/hw1.0/firmware-6.bin",
     "ath10k/QCA9377/hw1.0/board-2.bin"}
};
unsigned wifi_port_target_count(void) {
    return sizeof(targets) / sizeof(targets[0]);
}
const WifiPortTarget *wifi_port_target(unsigned index) {
    return index < wifi_port_target_count() ? &targets[index] : 0;
}
const WifiPortTarget *wifi_port_match(uint16_t vendor, uint16_t device) {
    for (unsigned i = 0; i < wifi_port_target_count(); i++)
        if (targets[i].vendor == vendor && targets[i].device == device)
            return &targets[i];
    return 0;
}
static int firmware_check(const char *name) {
    if (!name) return DRV_OK;
    DrvFirmware f = {0};
    int rc = drv_firmware_request(name, &f);
    if (rc) return rc;
    return drv_firmware_release(&f);
}
int wifi_port_preflight(const WifiPortTarget *target, unsigned instance,
                        WifiPortProbe *out) {
    if (!target || !out) return DRV_EINVAL;
    memset(out, 0, sizeof(*out));
    out->target = target;
    out->mmio_bar = 6;
    out->pci_result = drv_pci_find(target->vendor, target->device, instance, &out->pci);
    if (out->pci_result) return out->pci_result;
    out->pci_present = true;
    /* PCI base class 0x02: do not probe other classes. */
    if ((out->pci.class_code >> 16) != 2) {
        out->pci_result = DRV_ENOTSUP;
        return DRV_ENOTSUP;
    }
    out->pci_result = drv_pci_claim(&out->pci);
    if (out->pci_result) return out->pci_result;
    /* BAR sizing only while the device is not bus-mastering. No radio
     * MMIO access, firmware upload, IRQ or DMA activation occurs here. */
    out->mmio_result = DRV_ENOENT;
    for (unsigned i = 0; i < 6; i++) {
        DrvBar bar;
        int rc = drv_pci_bar(&out->pci, i, &bar);
        if (!rc && !bar.io && bar.size) {
            out->mmio_bar = i;
            out->mmio_size = bar.size;
            out->mmio_result = DRV_OK;
            out->mmio_ready = true;
            break;
        }
        if (rc != DRV_ENOENT && rc != DRV_EINVAL)
            out->mmio_result = rc;
    }
    /* Read, check and release firmware only; not sent to the radio. */
    out->firmware_result = firmware_check(target->firmware);
    out->firmware_aux_result = firmware_check(target->firmware_aux);
    out->firmware_ready = !out->firmware_result && !out->firmware_aux_result;
    int release_rc = drv_pci_release(&out->pci);
    if (release_rc) {
        out->pci_result = release_rc;
        return release_rc;
    }
    /* driver_loaded is deliberately false until a real MAC/PHY driver exists. */
    return DRV_OK;
}

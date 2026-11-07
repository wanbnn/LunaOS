#include "rtw8822be_diag.h"
#include "driver.h"
#include "rtw8822b_native.h"
/* Read-only smoke diagnostic for Realtek RTL8822BE (PCI 10ec:b822).
 * The Linux rtw88 driver uses BAR2 and REG_SYS_CFG1 at offset 0x00f0.
 * This code DOES NOT initialize a radio, DMA, interrupts, firmware upload,
 * transmit queues, association, encryption, DHCP, or any wireless link. */
#define RTW_VENDOR 0x10ecu
#define RTW_DEVICE 0xb822u
#define RTW_BAR 2u
#define RTW_REG_SYS_CFG1 0x00f0u
#define RTW_FW "rtw88/rtw8822b_fw.bin"
static void code(char *out, size_t cap, const char *step, int rc) {
    char n[16];
    append(out, step, cap);
    append(out, " failed: ", cap);
    number(n, (uint32_t)(-rc));
    append(out, n, cap);
    append(out, "\n", cap);
}
static int native_read(void *ctx, uint32_t reg, unsigned width, uint32_t *out) {
    return drv_mmio_read(ctx, reg, width, out);
}
static int native_write(void *ctx, uint32_t reg, unsigned width, uint32_t value) {
    return drv_mmio_write(ctx, reg, width, value);
}
static int native_delay(void *ctx, unsigned us) {
    (void)ctx;
    return drv_udelay(us);
}
static int diagnose(char *out, size_t cap, bool power_test) {
    if (!out || !cap) return DRV_EINVAL;
    out[0] = 0;
    append(out, "RTL8822BE / rtw88: read-only PCI/MMIO diagnostic\n", cap);
    DrvPci pci = {0};
    int rc = drv_pci_find(RTW_VENDOR, RTW_DEVICE, 0, &pci);
    if (rc) { code(out, cap, "PCI discovery", rc); return rc; }
    if ((pci.class_code >> 8) != 0x0280u) {
        append(out, "Unexpected PCI class; probe refused.\n", cap);
        return DRV_ENOTSUP;
    }
    rc = drv_pci_claim(&pci);
    if (rc) { code(out, cap, "PCI claim", rc); return rc; }
    DrvMmio mmio = {0};
    DrvFirmware fw = {0};
    uint32_t command = 0;
    rc = drv_pci_config_read(&pci, 4, 2, &command);
    if (rc) { code(out, cap, "PCI command", rc); goto done; }
    DrvBar bar = {0};
    rc = drv_pci_bar(&pci, RTW_BAR, &bar);
    if (rc || bar.io || bar.size < RTW_REG_SYS_CFG1 + 4u) {
        if (!rc) rc = DRV_ENOTSUP;
        code(out, cap, "MMIO BAR2", rc);
        goto done;
    }
    append(out, "BAR2 size: ", cap);
    char n[20];
    number(n, bar.size);
    append(out, n, cap);
    append(out, " bytes\n", cap);
    /* Enable MMIO decode temporarily, but NOT bus mastering. Original PCI
     * command is restored by drv_pci_release after unmapping. */
    rc = drv_pci_enable(&pci, (command & 1u) != 0, true);
    if (rc) { code(out, cap, "MMIO decode", rc); goto done; }
    rc = drv_pci_map(&pci, RTW_BAR, &mmio);
    if (rc) { code(out, cap, "MMIO map", rc); goto done; }
    uint32_t sys_cfg = 0;
    rc = drv_mmio_read(&mmio, RTW_REG_SYS_CFG1, 4, &sys_cfg);
    if (rc) { code(out, cap, "REG_SYS_CFG1 read", rc); goto done; }
    append(out, "REG_SYS_CFG1 = ", cap);
    hex(n, sys_cfg);
    append(out, n, cap);
    append(out, "\n", cap);
    if (sys_cfg == UINT32_MAX) {
        rc = DRV_EIO;
        append(out, "MMIO returned all ones; device not responding.\n", cap);
        goto done;
    }
    rc = drv_firmware_request(RTW_FW, &fw);
    if (rc) { code(out, cap, "Firmware rtw8822b_fw.bin", rc); goto done; }
    RtwFirmwareImage image;
    if (rtw8822b_firmware_parse(fw.data, fw.size, &image)) {
        rc = DRV_EIO;
        append(out, "Invalid RTL8822B firmware header/segments.\n", cap);
        goto done;
    }
    append(out, "Firmware present (bytes): ", cap);
    number(n, fw.size);
    append(out, n, cap);
    append(out, "\nFirmware validated: version=", cap);
    number(n, image.version); append(out, n, cap);
    append(out, " h2c=", cap); number(n, image.h2c_version); append(out, n, cap);
    append(out, " segments=", cap); number(n, image.count); append(out, n, cap);
    append(out, "\nRadio NOT initialized; no scan, WPA2 or wireless IP.\n", cap);
    if (power_test) {
        RtwRegisterIo io = { .ctx=&mmio, .read=native_read,
                            .write=native_write, .delay_us=native_delay };
        append(out, "MAC power cycle (RF/DMA stay disabled):\n", cap);
        rc = rtw8822b_mac_power_on(&io, (sys_cfg >> 12) & 15);
        uint32_t failed_register = io.failed_register;
        if (rc == DRV_EALREADY) {
            append(out, "MAC already active; refused to seize it. No power writes performed.\n", cap);
        } else if (rc && !io.writes_started) {
            code(out, cap, "MAC validation/power-on", rc);
        } else {
            if (rc) {
                code(out, cap, "MAC power-on", rc);
                append(out, "Register: ", cap); hex(n, failed_register); append(out, n, cap);
                append(out, "\n", cap);
            } else append(out, "MAC power-on sequence complete.\n", cap);
            int off_rc = rtw8822b_mac_power_off(&io, (sys_cfg >> 12) & 15);
            if (off_rc) {
                code(out, cap, "MAC power-off", off_rc);
                append(out, "Power state uncertain; reboot before retry.\n", cap);
                if (!rc) rc = off_rc;
            } else append(out, "MAC powered off; no wireless link established.\n", cap);
        }
    }
done:
    if (fw.data) {
        int r = drv_firmware_release(&fw);
        if (!rc) rc = r;
    }
    if (mmio.cookie) {
        int r = drv_mmio_unmap(&mmio);
        if (!rc) rc = r;
    }
    /* Never drop ownership if the MMIO mapping could not be released. */
    if (!mmio.cookie) {
        int r = drv_pci_release(&pci);
        if (!rc) rc = r;
    }
    return rc;
}

int rtw8822be_diag(char *out, size_t cap) { return diagnose(out, cap, false); }
int rtw8822be_power_test(char *out, size_t cap) { return diagnose(out, cap, true); }

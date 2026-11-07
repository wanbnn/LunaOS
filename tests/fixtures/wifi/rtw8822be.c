/* SPDX-License-Identifier: GPL-2.0-or-later
 * Test-only partial RTL8822BE model. Register/descriptor contracts: Linux
 * v6.12 drivers/net/wireless/realtek/rtw88/{pci,fw,rx}. No MCU execution.
 * Only firmware transfer, power, efuse, RF tuning and passive RX are modeled.
 * Input is raw 802.11 in classic little-endian PCAP, linktype 105.
 */
#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "migration/vmstate.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qom/object.h"
#define TYPE_LAB_RTW "lab-rtw8822be"
OBJECT_DECLARE_SIMPLE_TYPE(LabRtw, LAB_RTW)
struct LabRtw {
    PCIDevice parent_obj;
    MemoryRegion bar;
    uint8_t regs[0x4000], efuse[1024], reserved[4096];
    char *firmware, *capture;
    gchar *fw, *pcap;
    gsize fwlen, pcaplen;
    uint32_t segaddr[3], segoff[3], seglen[3], uploaded[3];
    unsigned nseg, reserved_len, wp, tag;
    size_t cursor;
    bool bad, running, stall_firmware;
    QEMUTimer *timer;
};
static uint32_t regget(LabRtw *s, unsigned a, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) { v |= (uint32_t)s->regs[a+i] << (8*i); }
    return v;
}
static void regput(LabRtw *s, unsigned a, unsigned n, uint32_t v)
{
    for (unsigned i = 0; i < n; i++) { s->regs[a+i] = v >> (8*i); }
}
static bool dma_read(LabRtw *s, uint32_t a, void *p, unsigned n)
{
    return (pci_get_word(s->parent_obj.config + PCI_COMMAND) & PCI_COMMAND_MASTER)
        && pci_dma_read(&s->parent_obj, a, p, n) == MEMTX_OK;
}
static bool dma_write(LabRtw *s, uint32_t a, const void *p, unsigned n)
{
    return (pci_get_word(s->parent_obj.config + PCI_COMMAND) & PCI_COMMAND_MASTER)
        && pci_dma_write(&s->parent_obj, a, p, n) == MEMTX_OK;
}
static void reserved_page(LabRtw *s)
{
    uint8_t bd[16], desc[48];
    s->reserved_len = 0;
    if (!dma_read(s, regget(s, 0x308, 4), bd, sizeof(bd)) ||
        lduw_le_p(bd) != 48 || !(lduw_le_p(bd+2) & 0x8000) ||
        !dma_read(s, ldl_le_p(bd+4), desc, sizeof(desc))) { return; }
    unsigned len = lduw_le_p(bd+8), checksum = 0;
    for (unsigned i = 0; i < 32; i += 2) { checksum ^= lduw_le_p(desc+i); }
    if (!len || len > sizeof(s->reserved) || checksum ||
        (ldl_le_p(desc) & 0xffff) != len ||
        ((ldl_le_p(desc+4) >> 8) & 31) != 16 ||
        !dma_read(s, ldl_le_p(bd+12), s->reserved, len)) { return; }
    s->reserved_len = len;
    regput(s, 0x204, 2, regget(s, 0x204, 2) | 0x8000);
}
static void iddma(LabRtw *s, uint32_t ctrl)
{
    unsigned len = ctrl & 0x3ffff;
    uint32_t addr = regget(s, 0x1204, 4);
    bool ok = regget(s, 0x1200, 4) == 0x18780030 && len &&
              len == s->reserved_len;
    unsigned i;
    for (i = 0; i < s->nseg; i++) {
        if (addr >= s->segaddr[i] && addr-s->segaddr[i] < s->seglen[i]) { break; }
    }
    if (i == s->nseg) { ok = false; }
    if (ok) {
        unsigned off = addr-s->segaddr[i];
        ok = off == s->uploaded[i] && len <= s->seglen[i]-off &&
             !memcmp(s->reserved, s->fw+s->segoff[i]+off, len);
        if (ok) { s->uploaded[i] += len; }
    }
    s->bad |= !ok;
    regput(s, 0x1208, 4, (ctrl & ~(1u<<31)) | (ok ? 0 : 1u<<27));
    s->reserved_len = 0;
}
static uint64_t mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    LabRtw *s = opaque;
    if (addr+size > sizeof(s->regs)) { return 0; }
    return regget(s, addr, size);
}
static void mmio_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    LabRtw *s = opaque;
    if (addr+size > sizeof(s->regs)) { return; }
    if (addr == 0x204 && size == 2) {
        regput(s, addr, size, value & ~0x8000); return; /* BCN_VALID W1C */
    }
    if (addr == 0x3b4 && size == 4) { value = (value & 0xffff) | (s->wp<<16); }
    regput(s, addr, size, value);
    if (addr == 5 && size == 1) { s->regs[5] &= ~3; }
    if (addr == 0x30 && size == 4 && !(value & (1u<<31))) {
        unsigned index = (value >> 8) & 1023;
        regput(s, addr, 4, (value & ~255u) | s->efuse[index] | (1u<<31));
    }
    if (addr == 0x1700) { regput(s, addr, 4, value | (1u<<29)); }
    if ((addr == 0xc90 || addr == 0xe90) && size == 4) {
        unsigned base = addr == 0xc90 ? 0x2800 : 0x2c00;
        regput(s, base+((value>>20)&255)*4, 4, value & 0xfffff);
    }
    if (addr == 0x208) { s->regs[addr] &= ~1; }
    if (addr == 0x338) { s->wp = s->tag = 0; regput(s, 0x3b4, 4, 0); }
    if (addr == 0x383 && (value & 16)) { reserved_page(s); s->regs[addr] &= ~16; }
    if (addr == 0x1208 && (value & (1u<<31))) { iddma(s, value); }
    if (addr == 3 && (value & 4) && (regget(s, 0x80, 2) & 0x4000)) {
        bool complete = !s->bad && !s->stall_firmware;
        for (unsigned i = 0; i < s->nseg; i++) {
            complete &= s->uploaded[i] == s->seglen[i];
        }
        if (complete) { s->running = true; regput(s, 0x80, 2, 0xc078); }
    }
}
static const MemoryRegionOps mmio_ops = {
    .read = mmio_read, .write = mmio_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};
static void receive_tick(void *opaque)
{
    LabRtw *s = opaque;
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)+20);
    if (!s->running || !s->pcap || !(s->regs[0x100] & 0x80) || s->regs[0x303]) { return; }
    unsigned count = regget(s, 0x382, 2) & 0xfff;
    unsigned rp = regget(s, 0x3b4, 2) & 0xfff;
    if (count < 2 || count > 4096 || rp >= count || (s->wp+1)%count == rp) { return; }
    if (s->cursor+16 > s->pcaplen) { s->cursor = 24; }
    const uint8_t *p = (uint8_t *)s->pcap+s->cursor;
    unsigned len = ldl_le_p(p+8);
    s->cursor += 16+len;
    p += 16;
    /* Only beacon frames on the tuned channel; PCAP is validated at realize. */
    if (len < 36 || len > 2304 || p[0] != 0x80) { return; }
    unsigned channel = 0;
    for (unsigned i = 36; i+2 <= len;) {
        unsigned bytes = p[i+1];
        if (i+2+bytes > len) { return; }
        if (p[i] == 3 && bytes == 1) { channel = p[i+2]; }
        i += 2+bytes;
    }
    if (!channel || channel != (regget(s, 0x2860, 4) & 255)) { return; }
    uint8_t bd[8], packet[24+32+2304+4] = {0};
    uint32_t ring = regget(s, 0x338, 4);
    if (!ring || !dma_read(s, ring+s->wp*8, bd, sizeof(bd)) ||
        lduw_le_p(bd) < 24+32+len+4) { return; }
    stl_le_p(packet, (len+4) | (4u<<16) | (1u<<26));
    packet[24] = 0; packet[25] = 65; /* PHY page 0, -45dBm */
    memcpy(packet+56, p, len);
    if (!dma_write(s, ldl_le_p(bd+4), packet, 56+len+4)) { return; }
    stw_le_p(bd+2, s->tag);
    if (!dma_write(s, ring+s->wp*8, bd, sizeof(bd))) { return; }
    s->tag = (s->tag+1)%8192; s->wp = (s->wp+1)%count;
    regput(s, 0x3b4, 4, rp | (s->wp<<16));
}
static void realize(PCIDevice *pdev, Error **errp)
{
    LabRtw *s = LAB_RTW(pdev);
    GError *err = NULL;
    if (!s->firmware || !g_file_get_contents(s->firmware, &s->fw, &s->fwlen, &err)) {
        error_setg(errp, "firmware reference required: %s", err ? err->message : "missing firmware property");
        g_clear_error(&err); return;
    }
    if (s->fwlen < 64 || s->fwlen > 512*1024 || lduw_le_p(s->fw) != 0x8822) {
        error_setg(errp, "invalid RTL8822B firmware reference"); return;
    }
    s->nseg = (((uint8_t *)s->fw)[24] & 16) && ldl_le_p(s->fw+52) ? 3 : 2;
    unsigned offset = 64;
    const unsigned sizes[] = {36,48,52}, addresses[] = {32,60,56};
    for (unsigned i = 0; i < s->nseg; i++) {
        uint32_t n = ldl_le_p(s->fw+sizes[i]);
        if (!n || offset > s->fwlen || s->fwlen-offset < 8 || n > s->fwlen-offset-8) {
            error_setg(errp, "truncated firmware segment"); return;
        }
        s->segaddr[i] = ldl_le_p(s->fw+addresses[i]) & 0x7fffffff;
        s->seglen[i] = n+8; s->segoff[i] = offset; offset += n+8;
    }
    if (offset != s->fwlen) { error_setg(errp, "firmware trailing bytes"); return; }
    if (s->capture) {
        if (!g_file_get_contents(s->capture, &s->pcap, &s->pcaplen, &err)) {
            error_setg(errp, "capture: %s", err->message); g_clear_error(&err); return;
        }
        if (s->pcaplen < 24 || ldl_le_p(s->pcap) != 0xa1b2c3d4 ||
            lduw_le_p(s->pcap+4) != 2 || lduw_le_p(s->pcap+6) != 4 ||
            ldl_le_p(s->pcap+20) != 105) {
            error_setg(errp, "capture must be classic LE PCAP, raw 802.11 (DLT 105)"); return;
        }
        for (size_t off = 24; off < s->pcaplen;) {
            if (s->pcaplen-off < 16) { error_setg(errp, "truncated PCAP header"); return; }
            unsigned n = ldl_le_p(s->pcap+off+8);
            if (!n || n > 2304 || n > s->pcaplen-off-16) {
                error_setg(errp, "invalid PCAP packet size"); return;
            }
            off += 16+n;
        }
        if (s->pcaplen == 24) { error_setg(errp, "empty capture"); return; }
    }
    s->cursor = 24;
    memset(s->efuse, 255, sizeof(s->efuse));
    /* Extended block 25, word 1: logical 0xca=RFE2, 0xcb=ff. */
    s->efuse[0] = 0x2f; s->efuse[1] = 0x3d;
    s->efuse[2] = 2; s->efuse[3] = 255;
    regput(s, 0xf0, 4, (1u<<27) | (1u<<12));
    s->regs[0x100] = 0xea; s->regs[6] = 2;
    regput(s, 0x1700, 4, 1u<<29);
    pci_config_set_interrupt_pin(pdev->config, 1);
    memory_region_init_io(&s->bar, OBJECT(s), &mmio_ops, s, "rtl8822be-lab-bar2", 0x4000);
    pci_register_bar(pdev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);
    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, receive_tick, s);
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)+20);
}
static void unrealize(PCIDevice *pdev)
{
    LabRtw *s = LAB_RTW(pdev);
    if (s->timer) { timer_free(s->timer); }
    g_free(s->fw); g_free(s->pcap);
}
static const Property properties[] = {
    DEFINE_PROP_STRING("firmware", LabRtw, firmware),
    DEFINE_PROP_STRING("capture", LabRtw, capture),
    DEFINE_PROP_BOOL("stall-firmware", LabRtw, stall_firmware, false),
};
static const VMStateDescription lab_vmstate = {
    .name = TYPE_LAB_RTW, .unmigratable = true,
};
static void class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    pc->realize = realize; pc->exit = unrealize;
    pc->vendor_id = 0x10ec; pc->device_id = 0xb822;
    pc->revision = 0; pc->class_id = PCI_CLASS_NETWORK_OTHER;
    pc->subsystem_vendor_id = 0x1b36; pc->subsystem_id = 0x8822;
    dc->desc = "Partial RTL8822BE laboratory model (passive RX; no RF/MCU execution)";
    dc->vmsd = &lab_vmstate;
    device_class_set_props(dc, properties);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}
static const TypeInfo info = {
    .name = TYPE_LAB_RTW, .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(LabRtw), .class_init = class_init,
    .interfaces = (const InterfaceInfo[]) { { INTERFACE_CONVENTIONAL_PCI_DEVICE }, {} },
};
static void register_types(void) { type_register_static(&info); }
type_init(register_types)

/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018-2019 Realtek Corporation
 * Native LunaOS adaptation of rtw88/mac.c power flow and fw.h firmware layout.
 * See assets/licenses/Realtek-rtw88-BSD.txt and docs/rtw8822be-native-port.md.
 */
#include "rtw8822b_native.h"
#define EINVAL (-22)
#define EIO (-5)
#define ENOTSUP (-95)
#define TIMEOUT (-110)
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16);
}
int rtw8822b_firmware_parse(const uint8_t *data, size_t bytes, RtwFirmwareImage *out) {
    if (!out) return EINVAL;
    *out = (RtwFirmwareImage){0};
    if (!data || bytes < 64 || bytes > 512u * 1024u) return EINVAL;
    if (le16(data) != 0x8822) return ENOTSUP;
    RtwFirmwareImage image = {0};
    image.version = le16(data + 4);
    image.subversion = data[6]; image.subindex = data[7];
    image.h2c_version = le16(data + 28);
    const unsigned size_offsets[] = {36, 48, 52}, address_offsets[] = {32, 60, 56};
    uint32_t cursor = 64;
    image.count = (data[24] & 16) && le32(data + 52) ? 3 : 2;
    for (unsigned i = 0; i < image.count; ++i) {
        uint32_t size = le32(data + size_offsets[i]);
        if (!size || size > bytes - cursor || bytes - cursor - size < 8) return EINVAL;
        uint32_t address = le32(data + address_offsets[i]) & 0x7fffffff;
        if (address > UINT32_MAX - size - 8) return EINVAL;
        image.segments[i] = (RtwFirmwareSegment){cursor, size + 8, address};
        cursor += size + 8;
    }
    if (cursor != bytes) return EINVAL;
    *out = image;
    return 0;
}
/* rtw88/rx.h: descriptor bit positions, bounded for caller-owned DMA buffers. */
int rtw8822b_rx_parse(const uint8_t *buffer, size_t bytes, RtwRxPacket *out) {
    if (!out) return EINVAL;
    *out = (RtwRxPacket){0};
    if (!buffer || bytes < 24) return EINVAL;
    uint32_t w0 = le32(buffer), w2 = le32(buffer + 8);
    unsigned length = w0 & 0x3fff, phy = ((w0 >> 16) & 15) * 8;
    unsigned shift = (w0 >> 24) & 3, offset = 24 + shift + phy;
    bool has_phy = (w0 & (1u << 26)) != 0, c2h = (w2 & (1u << 28)) != 0;
    if (w0 & (3u << 14)) return EIO; /* CRC/ICV error: never deliver. */
    if (offset > bytes || length > bytes - offset || !length || length > 11454)
        return EINVAL;
    if ((phy && phy != 32) || (has_phy && !phy)) return EINVAL;
    unsigned rate = le32(buffer + 12) & 0x7f;
    unsigned bw = (le32(buffer + 16) >> 4) & 3;
    if (!c2h && (rate >= 0x54 || bw > 2 || length <= 4)) return EINVAL;
    *out = (RtwRxPacket){ .frame=buffer + offset,
        .phy=has_phy ? buffer + 24 + shift : NULL,
        .frame_bytes=length - (c2h ? 0 : 4), .phy_bytes=has_phy ? phy : 0,
        .rate=rate, .bandwidth=bw, .c2h=c2h,
        .decrypted=!(w0 & (1u << 27)) && ((w0 >> 20) & 7) != 0 };
    return 0;
}
typedef struct {
    uint16_t offset;
    uint8_t cut_mask, intf_mask, base, cmd, mask, value;
} RtwPowerCommand;
#include "rtw8822b_power_tables.h"
static int read_reg(RtwRegisterIo *io, uint32_t reg, unsigned width, uint32_t *v) {
    int rc = io->read(io->ctx, reg, width, v);
    if (rc) io->failed_register = reg;
    return rc;
}
static int write_reg(RtwRegisterIo *io, uint32_t reg, unsigned width, uint32_t v) {
    io->writes_started = true;
    int rc = io->write(io->ctx, reg, width, v);
    if (rc) io->failed_register = reg;
    return rc;
}
static int update(RtwRegisterIo *io, uint32_t reg, unsigned width,
                  uint32_t clear, uint32_t set) {
    uint32_t v;
    int rc = read_reg(io, reg, width, &v);
    return rc ? rc : write_reg(io, reg, width, (v & ~clear) | set);
}
static int poll(RtwRegisterIo *io, const RtwPowerCommand *c) {
    for (unsigned retry = 0; retry < 2; ++retry) {
        for (unsigned i = 0; i < 20000; ++i) {
            uint32_t v;
            int rc = read_reg(io, c->offset, 1, &v);
            if (rc) return rc;
            if ((v & c->mask) == (c->value & c->mask)) return 0;
            rc = io->delay_us(io->ctx, 50);
            if (rc) return rc;
        }
        if (!retry) {
            /* Upstream PCIe retry: toggle PFM_WOWL, then repeat polling. */
            int rc = update(io, 4, 1, 0, 8);
            if (!rc) rc = update(io, 4, 1, 8, 0);
            if (rc) return rc;
        }
    }
    io->failed_register = c->offset;
    return TIMEOUT;
}
static int sequence(RtwRegisterIo *io, unsigned cut,
                    const RtwPowerCommand *const *flow) {
    for (unsigned f = 0; flow[f]; ++f) {
        const RtwPowerCommand *c = flow[f];
        for (; c->cmd != RTW_PWR_CMD_END; ++c) {
            if (!(c->intf_mask & 4u) ||
                !(c->cut_mask & (1u << (cut + 1)))) continue;
            if (c->base != RTW_PWR_ADDR_MAC) return ENOTSUP;
            int rc;
            switch (c->cmd) {
            case RTW_PWR_CMD_WRITE:
                rc = update(io, c->offset, 1, c->mask, c->value & c->mask); break;
            case RTW_PWR_CMD_POLLING: rc = poll(io, c); break;
            case RTW_PWR_CMD_DELAY:
                rc = io->delay_us(io->ctx, c->offset * (c->value == RTW_PWR_DELAY_MS ? 1000u : 1u));
                break;
            default: return EINVAL;
            }
            if (rc) return rc;
        }
    }
    return 0;
}
static int valid_io(RtwRegisterIo *io, unsigned cut) {
    if (!io || !io->read || !io->write || !io->delay_us || cut > 6) return EINVAL;
    io->failed_register = UINT32_MAX;
    io->writes_started = false;
    uint32_t id;
    int rc = read_reg(io, 0xf0, 4, &id);
    if (rc) return rc;
    if (!id || id == UINT32_MAX) return EIO;
    if (((id >> 12) & 15) != cut) return EINVAL;
    return 0;
}
int rtw8822b_mac_power_off(RtwRegisterIo *io, unsigned cut) {
    int rc = valid_io(io, cut);
    if (rc) return rc;
    uint32_t cr;
    rc = read_reg(io, 0x100, 1, &cr);
    if (rc || cr == 0xea) return rc;
    return sequence(io, cut, card_disable_flow_8822b);
}
int rtw8822b_mac_power_on(RtwRegisterIo *io, unsigned cut) {
    int rc = valid_io(io, cut);
    if (rc) return rc;
    uint32_t cr;
    rc = read_reg(io, 0x100, 1, &cr);
    if (rc) return rc;
    if (cr != 0xea) return -114; /* Refuse to seize an already active MAC. */
    /* PCIe-only pre-system configuration from rtw88/mac.c. BB/RF remains off. */
#define TRY(expr) do { rc = (expr); if (rc) return rc; } while (0)
    TRY(write_reg(io, 0x1c, 1, 0));
    TRY(update(io, 0x74, 4, 0, 1u << 8));
    TRY(update(io, 0x64, 4, 0, (1u << 29) | (1u << 28)));
    TRY(update(io, 0x4c, 4, (1u << 26) | (1u << 25), 0));
    TRY(update(io, 0x40, 4, 0, 4));
    TRY(update(io, 2, 1, 3, 0));
    TRY(update(io, 0x1f, 1, 7, 0));
    TRY(update(io, 0xec, 4, 7u << 24, 0));
    /* Resume a firmware left running by a previous OS, per rtw88. */
    uint32_t fw, rpwm;
    TRY(read_reg(io, 0x80, 2, &fw));
    if (fw == 0xc078) {
        TRY(read_reg(io, 0x3d9, 1, &rpwm));
        TRY(write_reg(io, 0x3d9, 1, (rpwm ^ 0x80) & 0x80));
    }
    TRY(sequence(io, cut, card_enable_flow_8822b));
    TRY(update(io, 0x1080, 4, 0, (1u << 16) | (1u << 8)));
    TRY(update(io, 3, 1, 0, 0xdc));
    TRY(update(io, 0x1103, 1, 15, 12));
    TRY(read_reg(io, 0x80, 4, &fw));
    if (fw & (1u << 20)) {
        TRY(write_reg(io, 0x80, 4, fw & ~(1u << 20)));
        TRY(update(io, 0x40, 4, 1u << 19, 0));
    }
#undef TRY
    return 0;
}

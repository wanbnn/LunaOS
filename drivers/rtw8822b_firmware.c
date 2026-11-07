/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018-2019 Realtek Corporation
 * Adapted from Linux rtw88/mac.c and util.c, pinned in assets/driver-sources.
 * See assets/licenses/Realtek-rtw88-BSD.txt.
 */
#include "rtw8822b_firmware.h"
#define EINVAL (-22)
#define EIO (-5)
#define TIMEOUT (-110)
#define BIT(n) (1u << (n))
static int rd(RtwFirmwareTransfer *t, uint32_t reg, unsigned width, uint32_t *v) {
    int rc = t->io->read(t->io->ctx, reg, width, v);
    if (rc) t->io->failed_register = reg;
    return rc;
}
static int wr(RtwFirmwareTransfer *t, uint32_t reg, unsigned width, uint32_t v) {
    t->io->writes_started = true;
    int rc = t->io->write(t->io->ctx, reg, width, v);
    if (rc) t->io->failed_register = reg;
    return rc;
}
static int update(RtwFirmwareTransfer *t, uint32_t reg, unsigned width,
                  uint32_t clear, uint32_t set) {
    uint32_t v;
    int rc = rd(t, reg, width, &v);
    return rc ? rc : wr(t, reg, width, (v & ~clear) | set);
}
static int wait_bits(RtwFirmwareTransfer *t, uint32_t reg, uint32_t mask, uint32_t expected) {
    for (unsigned i = 0; i < 1000; ++i) {
        uint32_t v;
        int rc = rd(t, reg, 4, &v);
        if (rc) return rc;
        if ((v & mask) == expected) return 0;
        rc = t->io->delay_us(t->io->ctx, 10);
        if (rc) return rc;
    }
    t->io->failed_register = reg;
    return TIMEOUT;
}
static int cpu(RtwFirmwareTransfer *t, bool on) {
    int rc;
    if (on) {
        rc = update(t, 0x1d, 1, 0, 1);
        return rc ? rc : update(t, 3, 1, 0, 4);
    }
    rc = update(t, 3, 1, 4, 0);
    /* Always attempt both disable writes, even after a bus error. */
    int second = update(t, 0x1d, 1, 1, 0);
    return rc ? rc : second;
}
static int segment(RtwFirmwareTransfer *t, const uint8_t *data,
                   const RtwFirmwareSegment *s) {
    int rc = wait_bits(t, 0x1208, BIT(31), 0);
    if (rc) return rc;
    rc = update(t, 0x1208, 4, 0, BIT(25)); /* reset segment checksum status */
    if (rc) return rc;
    for (unsigned offset = 0; offset < s->bytes;) {
        unsigned bytes = s->bytes - offset;
        if (bytes > 4096) bytes = 4096;
        rc = t->send_reserved(t->tx_context, data + s->offset + offset, bytes);
        if (rc) return rc;
        rc = wait_bits(t, 0x1208, BIT(31), 0);
        if (rc) return rc;
        rc = wr(t, 0x1200, 4, 0x18780000u + 48); /* TX FIFO + TX packet descriptor */
        if (!rc) rc = wr(t, 0x1204, 4, s->device_address + offset);
        uint32_t ctrl = BIT(31) | BIT(29) | bytes;
        if (offset) ctrl |= BIT(24); /* continue this segment's checksum */
        if (!rc) rc = wr(t, 0x1208, 4, ctrl);
        if (!rc) rc = wait_bits(t, 0x1208, BIT(31), 0);
        if (rc) return rc;
        offset += bytes;
        t->transferred_bytes += bytes;
        t->transferred_chunks++;
    }
    uint32_t ctrl, fw;
    rc = rd(t, 0x1208, 4, &ctrl);
    if (!rc) rc = rd(t, 0x80, 1, &fw);
    if (rc) return rc;
    uint32_t downloaded = s->device_address < 0x200000 ? BIT(3) : BIT(5);
    uint32_t checksum = s->device_address < 0x200000 ? BIT(4) : BIT(6);
    fw |= downloaded;
    fw = ctrl & BIT(27) ? fw & ~checksum : fw | checksum;
    rc = wr(t, 0x80, 1, fw);
    return rc ? rc : ctrl & BIT(27) ? EIO : 0;
}
int rtw8822b_firmware_download(RtwFirmwareTransfer *t, const uint8_t *data, size_t bytes) {
    if (!t) return EINVAL;
    t->firmware_running = false;
    t->transferred_bytes = t->transferred_chunks = 0;
    t->cleanup_error = 0;
    if (!t->io || !t->io->read || !t->io->write || !t->io->delay_us || !t->send_reserved)
        return EINVAL;
    t->io->failed_register = UINT32_MAX;
    t->io->writes_started = false;
    RtwFirmwareImage image;
    int rc = rtw8822b_firmware_parse(data, bytes, &image);
    if (rc) return rc; /* no hardware writes for malformed input */
    struct { uint32_t reg, value; unsigned width; } backup[] = {
        {0x10d,0,1}, {0x100,0,1}, {0x1330,BIT(31),4},
        {0x230,0,2}, {0x22c,0,4}, {0x550,0,1}
    };
    for (unsigned i = 0; i < 6; ++i) {
        if (i == 2) continue; /* write-one-to-clear register, not an ordinary snapshot */
        rc = rd(t, backup[i].reg, backup[i].width, &backup[i].value);
        if (rc) return rc;
    }
    backup[4].value |= BIT(31);
    uint32_t coex;
    rc = wait_bits(t, 0x1700, BIT(29), BIT(29));
    if (!rc) rc = wr(t, 0x1700, 4, 0x800f0038);
    if (!rc) rc = rd(t, 0x1708, 4, &coex);
    if (rc) return rc;
    bool mutated = false;
#define TRY(expr) do { rc=(expr); if (rc) goto restore; } while (0)
    mutated = true;
    TRY(cpu(t, false));
    TRY(wr(t, 0x10d, 1, 3u << 6));
    TRY(wr(t, 0x100, 1, 5)); /* HCI TXDMA and device TXDMA, no MAC TX/RX */
    TRY(wr(t, 0x1330, 4, BIT(31)));
    TRY(wr(t, 0x230, 2, 0x200));
    TRY(wr(t, 0x22c, 4, backup[4].value));
    TRY(wr(t, 0x550, 1, (backup[5].value & ~8u) | 16));
    TRY(update(t, 0x1082, 1, 1, 0));
    TRY(update(t, 9, 1, 64, 0));
    TRY(update(t, 0x1082, 1, 0, 1));
    TRY(update(t, 9, 1, 0, 64));
    uint32_t fw;
    TRY(rd(t, 0x80, 2, &fw));
    TRY(wr(t, 0x80, 2, (fw & 0x3800) | 1));
    for (unsigned i = 0; i < image.count; ++i) TRY(segment(t, data, &image.segments[i]));
restore:
    if (mutated) {
        for (unsigned i = 0; i < 6; ++i) {
            int r = wr(t, backup[i].reg, backup[i].width, backup[i].value);
            if (r && !t->cleanup_error) t->cleanup_error = r;
        }
        if (!rc && t->cleanup_error) rc = t->cleanup_error;
    }
    if (!rc) {
        rc = wr(t, 0x210, 4, 4); /* TXDMA page overflow acknowledgement */
        if (!rc) rc = rd(t, 0x80, 2, &fw);
        if (!rc && (fw & 0x50) != 0x50) rc = EIO;
        if (!rc) rc = wr(t, 0x80, 2, (fw | 0x4000) & ~1u);
        if (!rc) rc = cpu(t, true);
    }
    int coex_rc = wait_bits(t, 0x1700, BIT(29), BIT(29));
    if (!coex_rc) coex_rc = wr(t, 0x1704, 4, coex);
    if (!coex_rc) coex_rc = wr(t, 0x1700, 4, 0xc00f0038);
    if (coex_rc && !t->cleanup_error) t->cleanup_error = coex_rc;
    if (!rc) rc = coex_rc;
    if (!rc) rc = wait_bits(t, 0x80, 0xcfff, 0xc078);
    if (rc && mutated) {
        int r = update(t, 0x80, 1, 1, 0);
        if (r && !t->cleanup_error) t->cleanup_error = r;
        r = cpu(t, false); /* never execute a partially uploaded image */
        if (r && !t->cleanup_error) t->cleanup_error = r;
    }
    t->firmware_running = rc == 0;
#undef TRY
    return rc;
}

/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUNA_RTW8822B_NATIVE_H
#define LUNA_RTW8822B_NATIVE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Native bounded adaptations of rtw88 formats and MAC power sequences.
 * Powering the MAC does not initialize RF or establish a Wi-Fi link. */
typedef struct {
    uint32_t offset, bytes, device_address;
} RtwFirmwareSegment;
typedef struct {
    uint16_t version, h2c_version;
    uint8_t subversion, subindex;
    unsigned count;
    RtwFirmwareSegment segments[3]; /* DMEM, IMEM, optional EMEM; incl. checksum */
} RtwFirmwareImage;
int rtw8822b_firmware_parse(const uint8_t *, size_t, RtwFirmwareImage *);
typedef struct {
    const uint8_t *frame, *phy;
    uint16_t frame_bytes;
    uint8_t phy_bytes, rate, bandwidth;
    bool c2h, decrypted;
} RtwRxPacket;
/* Input starts at the 24-byte RX packet descriptor, not the RX buffer
 * descriptor. Output borrows storage; FCS is excluded from data frames. */
int rtw8822b_rx_parse(const uint8_t *, size_t, RtwRxPacket *);
typedef struct {
    void *ctx;
    int (*read)(void *, uint32_t offset, unsigned width, uint32_t *);
    int (*write)(void *, uint32_t offset, unsigned width, uint32_t);
    int (*delay_us)(void *, unsigned);
    uint32_t failed_register;
    bool writes_started;
} RtwRegisterIo;
/* On failure caller must attempt power_off before releasing the device.
 * These routines require sole PCI ownership, RF quiesced and DMA stopped. */
int rtw8822b_mac_power_on(RtwRegisterIo *, unsigned cut);
int rtw8822b_mac_power_off(RtwRegisterIo *, unsigned cut);
#endif

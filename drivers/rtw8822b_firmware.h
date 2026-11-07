/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef LUNA_RTW8822B_FIRMWARE_H
#define LUNA_RTW8822B_FIRMWARE_H
#include "rtw8822b_native.h"
typedef struct {
    RtwRegisterIo *io;
    void *tx_context;
    /* Copies to the device reserved page at page zero and waits for BCN_VALID.
     * Buffer must not be reused while PCIe DMA owns it. */
    int (*send_reserved)(void *, const uint8_t *, unsigned);
    unsigned transferred_bytes, transferred_chunks;
    int cleanup_error;
    bool firmware_running;
} RtwFirmwareTransfer;
/* Requires powered MAC and operational reserved-page PCIe TX; does not enable RF.
 * Readiness is set only after the device MCU acknowledges successful boot.
 * This function does not implement the PCIe DMA sender passed above. */
int rtw8822b_firmware_download(RtwFirmwareTransfer *, const uint8_t *, size_t);
#endif

#ifndef LUNA_BOOT_H
#define LUNA_BOOT_H
#include "luna.h"
#define LUNA_UEFI_MAGIC 0x55454649u
#define LUNA_UEFI_INFO ((const UefiBootInfo *)0x6f10)
#define LUNA_CORE_BYTES (32u * 1024 * 1024)
#define LUNA_ESP_BYTES (34u * 1024 * 1024)
#define LUNA_ESP_SECTOR (LUNA_CORE_BYTES / 512u)
typedef struct {
    uint32_t magic, version, ram_image, ram_bytes;
} UefiBootInfo;
static inline bool boot_is_uefi(void) {
    return LUNA_UEFI_INFO->magic == LUNA_UEFI_MAGIC && LUNA_UEFI_INFO->version == 1;
}
#endif

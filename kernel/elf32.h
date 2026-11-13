#ifndef LUNA_ELF32_H
#define LUNA_ELF32_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Static i386 ELF32, GDT-relative virtual addresses 0..64 KiB.
 * This is a limited native LunaOS ABI, NOT a Linux/POSIX ELF loader. */
#define LUNA_ELF_IMAGE_BYTES (64u * 1024u)
#define LUNA_ELF_PAGE_BYTES 4096u
#define LUNA_ELF_PAGES (LUNA_ELF_IMAGE_BYTES / LUNA_ELF_PAGE_BYTES)
#define LUNA_ELF_MAX_FILE_BYTES (256u * 1024u)
#define LUNA_ELF_MAX_SEGMENTS 16u
#define LUNA_ELF_READ 4u
#define LUNA_ELF_WRITE 2u
#define LUNA_ELF_EXEC 1u
typedef struct {
    uint8_t memory[LUNA_ELF_IMAGE_BYTES];
    uint8_t permissions[LUNA_ELF_PAGES]; /* 0 for unmapped, ELF PF_R/PF_W/PF_X */
    uint32_t entry;
    uint32_t segments;
} LunaElfImage;
bool luna_elf32_load(const uint8_t *bytes, size_t size, LunaElfImage *image);
#endif

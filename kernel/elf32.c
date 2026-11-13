#include "elf32.h"
#include <stdint.h>
#include "luna.h"

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
bool luna_elf32_load(const uint8_t *b, size_t n, LunaElfImage *image) {
    if (!b || !image || n < 52 || n > 1024u * 1024u)
        return false;
    if (b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F' ||
        b[4] != 1 || b[5] != 1 || b[6] != 1 || b[7] != 0)
        return false; /* ELFCLASS32, little endian, version 1, System V */
    uint16_t type = u16(b + 16);
    if ((type != 2 && type != 3) || u16(b + 18) != 3 || u32(b + 20) != 1 ||
        u16(b + 40) != 52 || u16(b + 42) != 32 || u16(b + 44) != 1)
        return false; /* Single segment static i386 executable only. */
    uint32_t off = u32(b + 28);
    if (off > n || n - off < 32)
        return false;
    const uint8_t *ph = b + off;
    if (u32(ph) != 1)
        return false; /* PT_LOAD */
    uint32_t source = u32(ph + 4), target = u32(ph + 8);
    uint32_t file_size = u32(ph + 16), mem_size = u32(ph + 20);
    uint32_t flags = u32(ph + 24), entry = u32(b + 24);
    if (!(flags & 1) || (flags & 2) || /* RX only, never user-writable code */
        file_size > mem_size ||
        source > n || file_size > n - source ||
        target >= LUNA_ELF_CODE_BYTES ||
        mem_size > LUNA_ELF_CODE_BYTES - target ||
        entry < target || entry - target >= mem_size)
        return false;
    /* Reject executable with an empty segment or malformed entry point. */
    if (!file_size || !mem_size)
        return false;
    memset(image->code, 0, sizeof(image->code));
    memcpy(image->code + target, b + source, file_size);
    image->entry = entry;
    return true;
}

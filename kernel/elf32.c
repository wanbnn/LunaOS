#include "elf32.h"
#include "luna.h"

static uint16_t u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
bool luna_elf32_inspect(const uint8_t *b, size_t available,
                        size_t file_bytes, LunaElfImage *image) {
    if (!b || !image || available < 84 || file_bytes < available ||
        file_bytes > LUNA_ELF_MAX_FILE_BYTES ||
        b[0] != 0x7f || b[1] != 'E' || b[2] != 'L' || b[3] != 'F' ||
        b[4] != 1 || b[5] != 1 || b[6] != 1 || b[7] != 0 ||
        u16(b + 16) != 2 || /* ET_EXEC only: ET_DYN needs a load bias */
        u16(b + 18) != 3 || u32(b + 20) != 1 ||
        u16(b + 40) != 52 || u16(b + 42) != 32)
        return false;
    uint32_t phoff = u32(b + 28), count = u16(b + 44), entry = u32(b + 24);
    if (!count || count > LUNA_ELF_MAX_SEGMENTS ||
        phoff > available || count * 32u > available - phoff)
        return false;
    memset(image, 0, sizeof(*image));
    bool executable_entry = false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = b + phoff + i * 32u;
        uint32_t type = u32(p);
        if (type != 1) {
            /* PT_NULL, PT_NOTE, PT_PHDR, non-exec GNU_STACK are metadata.
             * Reject PT_INTERP and PT_DYNAMIC until the real linker exists. */
            if (type == 0 || type == 4 || type == 6)
                continue;
            if (type == 0x6474e551u && !(u32(p + 24) & LUNA_ELF_EXEC))
                continue;
            return false;
        }
        uint32_t src = u32(p + 4), dest = u32(p + 8);
        uint32_t file_size = u32(p + 16), mem_size = u32(p + 20);
        uint32_t flags = u32(p + 24), align = u32(p + 28);
        if (!mem_size || file_size > mem_size ||
            src > file_bytes || file_size > file_bytes - src ||
            dest >= LUNA_ELF_IMAGE_BYTES ||
            mem_size > LUNA_ELF_IMAGE_BYTES - dest ||
            !(flags & LUNA_ELF_READ) || (flags & ~7u) ||
            ((flags & 3u) == 3u) || /* W+X forbidden */
            (align > LUNA_ELF_PAGE_BYTES) ||
            (align > 1 && ((align & (align - 1u)) ||
                           (dest & (align - 1u)) != (src & (align - 1u)))))
            return false;
        uint32_t first = dest / LUNA_ELF_PAGE_BYTES;
        uint32_t last = (dest + mem_size - 1u) / LUNA_ELF_PAGE_BYTES;
        for (uint32_t page = first; page <= last; page++) {
            if (image->permissions[page])
                return false; /* No ambiguous per-page permissions. */
            image->permissions[page] = (uint8_t)flags;
        }
        LunaElfSegment *s = &image->load[image->segments++];
        *s = (LunaElfSegment){src, dest, file_size, mem_size, flags,
                              first, last - first + 1u};
        if ((flags & LUNA_ELF_EXEC) && file_size &&
            entry >= dest && entry - dest < file_size)
            executable_entry = true;
    }
    if (!image->segments || !executable_entry)
        return false;
    image->entry = entry;
    return true;
}
bool luna_elf32_load(const uint8_t *b, size_t n, LunaElfImage *image) {
    return luna_elf32_inspect(b, n, n, image);
}

#ifndef LUNA_ELF32_H
#define LUNA_ELF32_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* LunaOS i386 static ELF loader. Segment-relative virtual addresses.
 * Two 4 MiB application PDEs; stack lives in a separate guarded PDE.
 * NOT a Linux ABI; no interpreter, shared libraries or relocations. */
#define LUNA_ELF_PAGE_BYTES 4096u
#define LUNA_ELF_IMAGE_BYTES (8u * 1024u * 1024u)
#define LUNA_ELF_PAGES (LUNA_ELF_IMAGE_BYTES / LUNA_ELF_PAGE_BYTES)
#define LUNA_ELF_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define LUNA_ELF_MAX_SEGMENTS 16u
#define LUNA_ELF_HEADER_BYTES 4096u
#define LUNA_ELF_STACK_OFFSET (LUNA_ELF_IMAGE_BYTES + 0x10000u)
#define LUNA_ELF_READ 4u
#define LUNA_ELF_WRITE 2u
#define LUNA_ELF_EXEC 1u
typedef struct {
    uint32_t file_offset, address, file_bytes, memory_bytes, flags;
    uint32_t first_page, page_count;
} LunaElfSegment;
typedef struct {
    uint8_t permissions[LUNA_ELF_PAGES]; /* zero => unmapped */
    LunaElfSegment load[LUNA_ELF_MAX_SEGMENTS];
    uint32_t entry, segments;
} LunaElfImage;

/* The first "available" bytes contain ELF and PHDR headers. The full file
 * may be much larger and is streamed into physical pages by the guest VFS. */
bool luna_elf32_inspect(const uint8_t *header, size_t available,
                        size_t file_bytes, LunaElfImage *image);
/* Host test convenience; does not copy/allocate the file's data segments. */
bool luna_elf32_load(const uint8_t *bytes, size_t size, LunaElfImage *image);
#endif

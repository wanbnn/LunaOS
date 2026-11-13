#ifndef LUNA_ELF32_H
#define LUNA_ELF32_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* LunaOS ELF32 MVP: 32-bit i386 ET_EXEC/ET_DYN with a single PT_LOAD.
 * A deliberately restricted binary format, not a POSIX ABI.
 * Virtual addresses in the ELF are offsets from USER_CODE and below 4096.
 * No dynamic linking, relocations, shared objects or multiple segments. */
#define LUNA_ELF_CODE_BYTES 4096u
typedef struct {
    uint8_t code[LUNA_ELF_CODE_BYTES];
    uint32_t entry;
} LunaElfImage;
bool luna_elf32_load(const uint8_t *bytes, size_t size, LunaElfImage *image);
#endif

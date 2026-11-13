#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/elf32.h"
static void w16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}
static void fixture(uint8_t *b, size_t length) {
    memset(b, 0, length);
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = 1; b[5] = 1; b[6] = 1;
    w16(b + 16, 2); w16(b + 18, 3); w32(b + 20, 1);
    w32(b + 24, 0); w32(b + 28, 52);
    w16(b + 40, 52); w16(b + 42, 32); w16(b + 44, 1);
    w32(b + 52, 1); w32(b + 56, 84); w32(b + 60, 0);
    w32(b + 68, 4); w32(b + 72, 8); w32(b + 76, 5);
    w32(b + 80, 1);
    b[84] = 0x90; b[85] = 0x90; b[86] = 0x90; b[87] = 0xc3;
}
int main(void) {
    uint8_t *b = malloc(10000);
    LunaElfImage *img = malloc(sizeof(*img));
    assert(b && img);
    fixture(b, 128);
    assert(luna_elf32_load(b, 128, img));
    assert(img->entry == 0 && img->segments == 1 &&
           img->load[0].file_offset == 84u && img->load[0].memory_bytes == 8u &&
           img->permissions[0] == 5 && !img->permissions[1]);
    assert(!luna_elf32_load(b, 60, img));
    b[4] = 2; assert(!luna_elf32_load(b, 128, img)); b[4] = 1;
    b[5] = 2; assert(!luna_elf32_load(b, 128, img)); b[5] = 1;
    w16(b + 18, 62); assert(!luna_elf32_load(b, 128, img)); w16(b + 18, 3);
    w32(b + 28, 127); assert(!luna_elf32_load(b, 128, img)); w32(b + 28, 52);
    w32(b + 60, 65535); w32(b + 72, 8);
    assert(!luna_elf32_load(b, 128, img));
    w32(b + 60, 0);
    w32(b + 56, 127); assert(!luna_elf32_load(b, 128, img)); w32(b + 56, 84);
    w32(b + 68, 100); assert(!luna_elf32_load(b, 128, img)); w32(b + 68, 4);
    w32(b + 24, 100); assert(!luna_elf32_load(b, 128, img)); w32(b + 24, 0);
    w32(b + 76, 7); assert(!luna_elf32_load(b, 128, img));
    fixture(b, 128); w16(b + 44, 2); assert(!luna_elf32_load(b, 128, img));

    /* An empty PT_LOAD from GNU ld must not prevent a read-only process. */
    fixture(b, 128);
    w16(b + 44, 2);
    w32(b + 84, 1); w32(b + 88, 128); w32(b + 92, 0x2000);
    w32(b + 100, 0); w32(b + 104, 0); w32(b + 108, 6);
    w32(b + 112, 1);
    assert(luna_elf32_load(b, 128, img));
    assert(img->segments == 1);
    w32(b + 100, 1);
    assert(!luna_elf32_load(b, 128, img));

    /* Valid multisegment, >4 KiB input, RX and RW separate page mappings. */
    fixture(b, 10000);
    w16(b + 44, 2);
    w32(b + 56, 4096); w32(b + 68, 8); w32(b + 72, 4096);
    w32(b + 84, 1);   /* second PT_LOAD */
    w32(b + 88, 8192); w32(b + 92, 0x4000);
    w32(b + 100, 16); w32(b + 104, 4096); w32(b + 108, 6);
    w32(b + 112, 1);
    b[4096] = 0x90; b[4097] = 0x90; b[4098] = 0x90; b[4099] = 0xc3;
    for (int i = 0; i < 16; i++) b[8192 + i] = (uint8_t)(i + 1);
    assert(luna_elf32_load(b, 10000, img));
    assert(img->segments == 2 && img->entry == 0);
    assert(img->permissions[0] == 5 && img->permissions[4] == 6);
    assert(!img->permissions[1] && !img->permissions[5]);
    assert(img->load[0].file_offset == 4096u && img->load[0].file_bytes == 8u &&
           img->load[1].file_offset == 8192u && img->load[1].memory_bytes == 4096u);

    /* No overlapping segments, no write+execute, no interpreter/dynamic. */
    w32(b + 92, 0); assert(!luna_elf32_load(b, 10000, img));
    w32(b + 92, 0x4000); w32(b + 108, 7);
    assert(!luna_elf32_load(b, 10000, img));
    w32(b + 108, 6); w32(b + 84, 3);
    assert(!luna_elf32_load(b, 10000, img));
    w32(b + 84, 1); w32(b + 88, 9990); w32(b + 100, 16);
    assert(!luna_elf32_load(b, 10000, img));
    w32(b + 88, 8192); w32(b + 92, LUNA_ELF_IMAGE_BYTES - 8u); w32(b + 104, 4096);
    assert(!luna_elf32_load(b, 10000, img));
    w32(b + 92, 0x4000); w32(b + 24, 0x4000); /* data cannot be entry */
    assert(!luna_elf32_load(b, 10000, img));
    /* Second PDE: a segment mapped above 5 MiB must be valid and sparse. */
    fixture(b, 10000);
    w16(b + 44, 2);
    w32(b + 56, 4096); w32(b + 68, 8); w32(b + 72, 4096);
    w32(b + 84, 1); w32(b + 88, 8192); w32(b + 92, 0x500000);
    w32(b + 100, 16); w32(b + 104, 4096); w32(b + 108, 6);
    w32(b + 112, 1);
    assert(luna_elf32_load(b, 10000, img));
    assert(img->permissions[0] == 5 && img->permissions[0x500000 / 4096] == 6);
    assert(!img->permissions[0x400000 / 4096]);

    /* Large file is inspected without allocating it or reading its payload. */
    w32(b + 88, 4u * 1024u * 1024u);
    assert(luna_elf32_inspect(b, 4096, 6u * 1024u * 1024u, img));
    assert(img->load[1].file_offset == 4u * 1024u * 1024u);
    assert(!luna_elf32_inspect(b, 128, 10000, img));
    w32(b + 92, LUNA_ELF_IMAGE_BYTES - 4096u);
    w32(b + 104, 8192);
    assert(!luna_elf32_inspect(b, 4096, 6u * 1024u * 1024u, img));
    free(img); free(b);
    puts("PASS: ELF32 multisegment, streamed 8 MiB virtual budget, >4 KiB files, W^X and bounds checks");
    return 0;
}

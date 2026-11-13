/* Early i386 LunaOS static ABI: two separate PT_LOAD segments.
 * Images use segment-relative addresses 0..64 KiB with no dynamic linker. */
OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0;
    .text : { *(.text.start) *(.text .text.*) *(.rodata .rodata.*) } :text
    . = ALIGN(4096);
    .data : { *(.data .data.*) *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) }
}

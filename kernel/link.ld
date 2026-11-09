ENTRY(_entry)
PHDRS {
 text PT_LOAD FLAGS(5);
 data PT_LOAD FLAGS(6);
}
SECTIONS {
 . = 0x100000;
 .text : { *(.text.entry) *(.text*) } :text
 .rodata : { *(.rodata*) } :text
 .eh_frame : ALIGN(4) {
  __eh_frame_start = .;
  KEEP(*litehtml_bridge.o(.eh_frame*))
  KEEP(*cxx_runtime.o(.eh_frame*))
  KEEP(*litehtml-float-progress.o(.eh_frame*))
  KEEP(*liblitehtml.a:*(.eh_frame*))
  KEEP(*libstdc++.a:*(.eh_frame*))
  KEEP(*libsupc++.a:*(.eh_frame*))
  KEEP(*libgcc_eh.a:*(.eh_frame*))
  LONG(0)
 } :text
 .data : ALIGN(16) { *(.data*) } :data
 .init_array : ALIGN(4) {
  __init_array_start = .;
  KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
  KEEP(*(.init_array))
  __init_array_end = .;
 } :data
 /* Never allocate buffers in the VGA aperture or BIOS ROM (0xa0000..0xfffff). */
 . = ALIGN(4096);
 __bss_start = .;
 .bss.low (NOLOAD) : { build/kernel/account.o(.bss) build/kernel/driver_services.o(.bss) build/kernel/fs.o(.bss) build/kernel/task.o(.bss) } :data
 __bss_low_end = .;
 /* Leave low firmware/ACPI allocations below 16 MiB intact. The UEFI
  * loader still validates the actual linked spans against its memory map. */
 . = MAX(., 0x1000000);
 __bss_high_start = .;
 .bss (NOLOAD) : { *(.bss*) *(COMMON) } :data
 __bss_end = .;
 /DISCARD/ : { *(.eh_frame*) *(.comment*) *(.note*) }
 ASSERT(__bss_end < 0x3d00000, "Kernel BSS overlaps stack")
}

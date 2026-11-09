#ifndef LUNA_IMAGE_LAYOUT_H
#define LUNA_IMAGE_LAYOUT_H
#define LUNA_IMAGE_BYTES (66u * 1024 * 1024)
/* Sector zero contains the MBR, followed by 64 sectors for the BIOS loader. */
#define LUNA_KERNEL_FIRST_SECTOR 65u
#define LUNA_KERNEL_SECTORS 16383u
#define LUNA_IMAGE_INFO_SECTOR 16383u
#define LUNA_VOLUME_START 16384u
#define LUNA_FIRMWARE_SECTOR 24576u
#endif

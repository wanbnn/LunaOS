#ifndef LUNA_DEVKIT_H
#define LUNA_DEVKIT_H
#include <stddef.h>
/* LunaOS Developer API v1: virtual devices only.
 * Hardware PCI/MMIO/DMA/IRQ drivers remain native C, statically linked. */
#define LDEV_MAX 8
int ldev_load(const char *descriptor);
int ldev_unload(const char *name);
int ldev_read(const char *name, char *out, size_t cap);
int ldev_write(const char *name, const char *value);
void ldev_list(char *out, size_t cap);
int ldev_valid_name(const char *name);
int developer_command(int argc, char **argv, char *out, size_t cap);
#endif

#ifndef LUNA_DEVKIT_H
#define LUNA_DEVKIT_H
#include <stddef.h>
#include <stdint.h>
/* LunaSDK v2: virtual endpoints plus safely registered native C endpoints.
 * C hardware drivers own PCI/MMIO/DMA/IRQ resources through kernel/driver.h. */
#define LDEV_MAX 8
typedef int (*LunaNativeRead)(void *ctx, char *out, size_t cap);
typedef int (*LunaNativeWrite)(void *ctx, const char *value);
/* Trusted kernel driver only. Native endpoints cannot be removed by guest scripts. */
int ldev_native_register(const char *name, LunaNativeRead read,
                         LunaNativeWrite write, void *ctx);
void lunasdk_edu_init(void);
int ldev_load(const char *descriptor);
int ldev_unload(const char *name);
int ldev_read(const char *name, char *out, size_t cap);
int ldev_write(const char *name, const char *value);
void ldev_list(char *out, size_t cap);
int ldev_valid_name(const char *name);
int developer_command(int argc, char **argv, char *out, size_t cap);
#endif

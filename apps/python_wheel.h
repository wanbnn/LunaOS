#ifndef LUNA_PYTHON_WHEEL_H
#define LUNA_PYTHON_WHEEL_H

#include "luna.h"

#define PYTHON_WHEEL_BYTES (4u * 1024 * 1024)
#define PYTHON_DIGEST_BYTES 32
#define PYTHON_INSTALL_RECORD "LUNA-RECORD"

bool python_package_normalize(const char *name, size_t len, char *out, size_t cap);
int python_fetch(const char *url, char **data, size_t *size, char *out, size_t cap);
bool python_wheel_name(const char *name, char *dist_info, size_t cap);
bool python_package_path(const char *path, size_t len);
bool python_wheel_install(const char *name, const uint8_t *zip, size_t size,
                          const char *library, bool no_deps, char *out, size_t cap);
bool python_package_find(const char *library, const char *project, char *dir, char *version);
void python_package_list(const char *library, bool freeze, char *out, size_t cap);
bool python_package_remove(const char *library, const char *dir, char *out, size_t cap);
bool python_index_find(const char *requirement, const char *index, char *url, size_t url_cap,
                       uint8_t digest[PYTHON_DIGEST_BYTES], char *out, size_t cap);

#endif

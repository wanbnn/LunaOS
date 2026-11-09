#ifndef LUNA_CPYTHON_PORT_H
#define LUNA_CPYTHON_PORT_H

#include "luna.h"
#include "wasm3.h"

#define CPYTHON_STDLIB "/lib/python313.zip"
#define WASI_FDS 32

typedef struct {
    char path[PATH_CAP];
    uint32_t position;
} WasiFd;

typedef struct {
    int argc;
    char **argv;
    char pythonpath[2 * PATH_CAP + 16];
    WasiFd descriptors[WASI_FDS];
    const uint8_t *stdlib;
    uint32_t stdlib_bytes;
    unsigned exit_status;
    char *out;
    size_t cap, used;
} WasiSession;

M3Result cpython_link_wasi(IM3Module module);

#endif

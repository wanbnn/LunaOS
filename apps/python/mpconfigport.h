#ifndef LUNA_MICROPYTHON_CONFIG_H
#define LUNA_MICROPYTHON_CONFIG_H

#include <alloca.h>

typedef long mp_off_t;

#define MICROPY_CONFIG_ROM_LEVEL        MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES
#define MICROPY_ENABLE_GC               (1)
#define MICROPY_PY_GC                   (1)
#define MICROPY_PY_IO                   (0)
#define MICROPY_STACK_CHECK             (1)
#define MICROPY_ENABLE_VM_ABORT         (1)
#define MICROPY_LONGINT_IMPL            MICROPY_LONGINT_IMPL_MPZ
#define MICROPY_FLOAT_IMPL              MICROPY_FLOAT_IMPL_DOUBLE
#define MICROPY_FLOAT_USE_NATIVE_FLT16 (0)
#define MICROPY_PY_SYS_PLATFORM         "lunaos"
#define MICROPY_PY_BUILTINS_STR_UNICODE (1)
#define MICROPY_MPHALPORT_H             "mphalport.h"
#define MP_PLAT_PRINT_STRN(str, len)    mp_hal_stdout_tx_strn(str, len)

void luna_python_poll(void);
#define MICROPY_VM_HOOK_LOOP luna_python_poll();

#endif

#include "luna.h"
#include "driver.h"
#include "heap.h"
#include "python.h"
#include "task.h"
#include <stdlib.h>
#include "py/compile.h"
#include "py/builtin.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/objlist.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "shared/runtime/gchelper.h"

#define PYTHON_HEAP_BYTES (2u * 1024 * 1024)
#define PYTHON_STACK_BYTES (48u * 1024)
#define PYTHON_TIMEOUT_TICKS 300
#define PYTHON_SOURCE_LIMIT (256u * 1024)

/* MicroPython's bare-metal port has a single VM state. */
static struct {
    uint32_t owner, deadline, last_yield;
    char *out;
    size_t cap, used;
} session;

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    size_t room = session.cap - session.used - 1;
    size_t copy = len < room ? len : room;
    for (size_t i = 0; i < copy; ++i)
        session.out[session.used + i] = str[i] ? str[i] : '?';
    session.used += copy;
    session.out[session.used] = 0;
    return len;
}

void gc_collect(void) {
    gc_collect_start();
    gc_helper_collect_regs_and_stack();
    gc_collect_end();
}

void luna_python_poll(void) {
    uint32_t now = ticks;
    if ((int32_t)(now - session.deadline) >= 0) {
        mp_sched_vm_abort();
    } else if (now != session.last_yield) {
        session.last_yield = now;
        task_yield_ready();
    }
}

void nlr_jump_fail(void *value) {
    (void)value;
    serial_write("MicroPython: exception escaped VM\n");
    abort();
}

mp_import_stat_t mp_import_stat(const char *path) {
    File *file = fs_find(path);
    if (!file)
        return MP_IMPORT_STAT_NO_EXIST;
    return file->dir ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    const char *path = qstr_str(filename);
    File *file = fs_find(path);
    if (!file || file->dir)
        mp_raise_OSError(MP_ENOENT);
    size_t len = file->size;
    if (len > PYTHON_SOURCE_LIMIT)
        mp_raise_OSError(MP_EFBIG);
    char *source = m_new(char, len + 1);
    if (!fs_read_bytes(file, 0, source, len))
        mp_raise_OSError(MP_EIO);
    return mp_lexer_new_from_str_len(filename, source, len, len + 1);
}

static int run_code(const char *source, const char *path, int argc, char **argv) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        nlr_set_abort(&nlr);
        if (path) {
            char dir[PATH_CAP];
            strcpy(dir, path);
            char *slash = strrchr(dir, '/');
            slash[slash == dir ? 1 : 0] = 0;
            mp_obj_t dir_obj = mp_obj_new_str(dir, strlen(dir));
            mp_obj_list_t *search = MP_OBJ_TO_PTR(mp_sys_path);
            search->items[0] = dir_obj;
            mp_obj_list_append(mp_sys_argv, mp_obj_new_str(path, strlen(path)));
        } else {
            mp_obj_list_append(mp_sys_argv, mp_obj_new_str("-c", 2));
        }
        for (int i = 0; i < argc; ++i)
            mp_obj_list_append(mp_sys_argv, mp_obj_new_str(argv[i], strlen(argv[i])));

        /* Older LunaPy scripts refer to the SDK without an import. */
        mp_store_global(MP_QSTR_luna, mp_import_name(MP_QSTR_luna, mp_const_none,
                                                     MP_OBJ_NEW_SMALL_INT(0)));
        qstr filename = path ? qstr_from_str(path) : MP_QSTR__lt_string_gt_;
        if (path)
            mp_store_global(MP_QSTR___file__, MP_OBJ_NEW_QSTR(filename));
        mp_lexer_t *lex = path ? mp_lexer_new_from_file(filename) :
            mp_lexer_new_from_str_len(filename, source, strlen(source), 0);
        mp_parse_tree_t tree = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t code = mp_compile(&tree, filename, false);
        mp_call_function_0(code);
        nlr_pop();
        return 0;
    }

    if (!nlr.ret_val) {
        mp_print_str(&mp_plat_print, "Python: execution timed out\n");
        return 1;
    }
    mp_obj_t error = MP_OBJ_FROM_PTR(nlr.ret_val);
    if (mp_obj_is_type(error, &mp_type_SystemExit)) {
        mp_obj_t value = mp_obj_exception_get_value(error);
        if (value == mp_const_none)
            return 0;
        if (mp_obj_is_int(value))
            return (uint8_t)mp_obj_get_int_truncated(value);
        mp_obj_print_helper(&mp_plat_print, value, PRINT_STR);
        mp_print_str(&mp_plat_print, "\n");
        return 1;
    }
    mp_obj_print_exception(&mp_plat_print, error);
    return 1;
}

static int execute(const char *source, const char *path, int argc, char **argv,
                   char *out, size_t cap) {
    uint32_t flags = drv_irq_save();
    Task *owner = session.owner ? task_by_pid(session.owner) : NULL;
    if (owner && owner->state != TASK_EXITED) {
        drv_irq_restore(flags);
        append(out, "Python: interpreter is busy\n", cap);
        return 1;
    }
    session.owner = task_current()->pid;
    drv_irq_restore(flags);

    void *heap = heap_allocate_owned(PYTHON_HEAP_BYTES, session.owner);
    if (!heap) {
        session.owner = 0;
        append(out, "Python: insufficient memory\n", cap);
        return 1;
    }
    session.out = out;
    session.cap = cap;
    session.used = strlen(out);
    session.last_yield = ticks;
    session.deadline = session.last_yield + PYTHON_TIMEOUT_TICKS;
    mp_stack_ctrl_init();
    mp_stack_set_limit(PYTHON_STACK_BYTES);
    gc_init(heap, (uint8_t *)heap + PYTHON_HEAP_BYTES);
    mp_init();
    int result = run_code(source, path, argc, argv);
    nlr_set_abort(NULL);
    mp_deinit();
    heap_release(heap);
    session.owner = 0;
    return result;
}

int python_run_file(const char *path, int argc, char **argv, char *out, size_t cap) {
    char full[PATH_CAP];
    if (!fs_path(path, full)) {
        append(out, "Python: invalid path\n", cap);
        return 2;
    }
    return execute(NULL, full, argc, argv, out, cap);
}

int python_command(int argc, char **argv, char *out, size_t cap) {
    if (argc == 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V"))) {
        append(out, "MicroPython " MICROPY_VERSION_STRING " on LunaOS\n", cap);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "-c"))
        return execute(argv[2], NULL, argc - 3, argv + 3, out, cap);
    if (argc >= 2 && argv[1][0] != '-')
        return python_run_file(argv[1], argc - 2, argv + 2, out, cap);
    append(out, "usage: python --version | -c CODE [ARGS...] | FILE.py [ARGS...]\n", cap);
    return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
}

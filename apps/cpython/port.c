#include "port.h"
#include "heap.h"
#include "image_layout.h"
#include "install.h"
#include "python.h"
#include "update.h"
#include "task.h"

#define BUNDLE_BYTES (5u * 1024 * 1024)
#define WASM_BYTES (8u * 1024 * 1024)
#define STDLIB_BYTES (16u * 1024 * 1024)
#define WASM_STACK_BYTES (128u * 1024)
#define PYTHON_TIMEOUT_MS 15000u

static const uint8_t wasm_digest[32] = {
    0xf8, 0xbf, 0xa1, 0x6f, 0x51, 0xc7, 0x95, 0x4d, 0xff, 0x8d, 0x77, 0x7a, 0x30, 0x5b, 0xa0, 0x58,
    0x6a, 0xe5, 0xd3, 0x77, 0x56, 0xa1, 0xf6, 0x76, 0xfc, 0x06, 0xa9, 0x34, 0xf5, 0x90, 0xfc, 0x5f,
};

static const uint8_t stdlib_digest[32] = {
    0x54, 0xc2, 0xb1, 0xdf, 0xf5, 0x1b, 0x58, 0x54, 0xdc, 0xd4, 0x2d, 0x6b, 0xce, 0xc7, 0x30, 0x10,
    0x8b, 0xc7, 0x82, 0x49, 0x6d, 0x84, 0x37, 0x12, 0xcf, 0xfb, 0x18, 0x5c, 0x30, 0xb0, 0x9f, 0x05,
};

static uint32_t owner, deadline, last_yield;
static uintptr_t stack_top;

M3Result m3_Yield(void) {
    if (!stack_top)
        return NULL;
    uint32_t now = frame_clock;
    if ((int32_t)(now - deadline) >= 0)
        return "execution timed out";
    uintptr_t stack = (uintptr_t)&stack;
    if (stack < stack_top && stack_top - stack > 256u * 1024)
        return "native stack limit exceeded";
    if (now - last_yield >= 20u) {
        last_yield = now;
        task_yield_ready();
    }
    return NULL;
}

static bool load_bundle(uint8_t **wasm, uint32_t *wasm_size, WasiSession *session) {
    const File disk = {.disk = disk_boot_index(), .size = BUNDLE_BYTES,
                       .disk_offset = (uint64_t)LUNA_CPYTHON_SECTOR * 512};
    struct {
        char magic[8];
        uint32_t packed_bytes, wasm_bytes, zip_packed_bytes, zip_bytes;
    } header;
    if (!fs_read_bytes(&disk, 0, &header, sizeof(header)) ||
        memcmp(header.magic, "LUNAPY3\0", 8)) {
        append(session->out, "CPython bundle unavailable on this boot disk.\n", session->cap);
        return false;
    }
    if (!header.wasm_bytes || header.wasm_bytes > WASM_BYTES ||
        !header.packed_bytes || header.packed_bytes >= BUNDLE_BYTES - 512 ||
        !header.zip_bytes || header.zip_bytes > STDLIB_BYTES ||
        !header.zip_packed_bytes || header.zip_packed_bytes > BUNDLE_BYTES - 512 - header.packed_bytes) {
        append(session->out, "CPython bundle header is invalid.\n", session->cap);
        return false;
    }
    char *packed = heap_allocate(header.packed_bytes);
    *wasm = heap_allocate(header.wasm_bytes);
    *wasm_size = header.wasm_bytes;
    bool ok = packed && *wasm &&
              fs_read_bytes(&disk, 512, packed, header.packed_bytes) &&
              update_inflate((char *)*wasm, header.wasm_bytes, packed, header.packed_bytes) ==
                  (int)header.wasm_bytes &&
              update_digest_valid(*wasm, header.wasm_bytes, wasm_digest);
    heap_release(packed);
    if (ok) {
        uint8_t *zip = heap_allocate(header.zip_bytes);
        session->stdlib = zip;
        session->stdlib_bytes = header.zip_bytes;
        packed = heap_allocate(header.zip_packed_bytes);
        ok = zip && packed && fs_read_bytes(&disk, 512 + header.packed_bytes, packed, header.zip_packed_bytes) &&
             update_inflate((char *)zip, header.zip_bytes, packed, header.zip_packed_bytes) ==
                 (int)header.zip_bytes && update_digest_valid(zip, header.zip_bytes, stdlib_digest);
        heap_release(packed);
    }
    if (!ok)
        append(session->out, "CPython bundle could not be loaded or verified.\n", session->cap);
    return ok;
}

static int run_python(WasiSession *session, const uint8_t *wasm, uint32_t size) {
    IM3Environment env = m3_NewEnvironment();
    IM3Runtime runtime = env ? m3_NewRuntime(env, WASM_STACK_BYTES, session) : NULL;
    IM3Module module = NULL;
    IM3Function start = NULL;
    M3Result result = "insufficient memory for WebAssembly runtime";
    if (runtime) {
        result = m3_ParseModule(env, &module, wasm, size);
        if (!result)
            result = m3_LoadModule(runtime, module);
        if (result) {
            if (module)
                m3_FreeModule(module);
        } else {
            result = cpython_link_wasi(module);
            if (!result)
                result = m3_FindFunction(&start, runtime, "_start");
            if (!result) {
                stack_top = (uintptr_t)&start;
                last_yield = frame_clock;
                deadline = last_yield + PYTHON_TIMEOUT_MS;
                result = m3_CallV(start);
                stack_top = 0;
            }
        }
        m3_FreeRuntime(runtime);
    }
    if (env)
        m3_FreeEnvironment(env);
    if (result && result != m3Err_trapExit) {
        append(session->out, "CPython WASI: ", session->cap);
        append(session->out, result, session->cap);
        append(session->out, "\n", session->cap);
        return 1;
    }
    return session->exit_status;
}

int cpython_command(int argc, char **argv, char *out, size_t cap) {
    if (argc >= 3 && !strcmp(argv[1], "-m") && !strcmp(argv[2], "pip"))
        return python_pip_command(argc - 2, argv + 2, out, cap);
    if (argc == 4 && !strcmp(argv[1], "-m") && !strcmp(argv[2], "venv"))
        return python_venv_create(argv[3], out, cap);
    if (argc < 2) {
        append(out, "usage: python --version | -c CODE | FILE.py | -m MODULE\n", cap);
        return 2;
    }
    if (memory_mb < 96) {
        append(out, "CPython needs more RAM. Use a 128 MiB VM or the micropython command.\n", cap);
        return 1;
    }
    WasiSession session = {.argc = argc, .argv = argv, .out = out, .cap = cap,
                           .used = strlen(out)};
    char library[PATH_CAP];
    if (!python_library_path(library)) {
        append(out, "Python: active environment is missing. Use venv deactivate.\n", cap);
        return 1;
    }
    strcpy(session.descriptors[3].path, "/");
    strcpy(session.pythonpath, "PYTHONPATH=");
    strcat(session.pythonpath, cwd);
    strcat(session.pythonpath, ":");
    strcat(session.pythonpath, library);

    char script[PATH_CAP];
    char *original = argv[1];
    if (argv[1][0] != '-') {
        if (!fs_path(argv[1], script)) {
            append(out, "Python: invalid script path.\n", cap);
            return 2;
        }
        argv[1] = script;
    }

    Task *previous = owner ? task_by_pid(owner) : NULL;
    if (previous && previous->state != TASK_EXITED) {
        argv[1] = original;
        append(out, "CPython: interpreter is busy.\n", cap);
        return 1;
    }
    owner = task_current()->pid;
    stack_top = 0;
    uint8_t *wasm = NULL;
    uint32_t wasm_size;
    int status = load_bundle(&wasm, &wasm_size, &session) ? run_python(&session, wasm, wasm_size) : 1;
    heap_release((void *)session.stdlib);
    heap_release(wasm);
    argv[1] = original;
    owner = 0;
    return status;
}

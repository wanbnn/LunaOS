/* Read-only WASI Preview 1 host for the pinned CPython distribution. */
#include "port.h"
#include "clock.h"
#include "web.h"
#include <string.h>

enum {
    WASI_OK = 0, WASI_BADF = 8, WASI_FAULT = 21, WASI_INVAL = 28, WASI_IO = 29,
    WASI_ISDIR = 31, WASI_MFILE = 33, WASI_NAMETOOLONG = 37, WASI_NOENT = 44,
    WASI_NOSYS = 52, WASI_NOTDIR = 54, WASI_ROFS = 69, WASI_SPIPE = 70,
};
enum { TYPE_CHARACTER = 2, TYPE_DIRECTORY = 3, TYPE_REGULAR = 4 };

static void store32(void *out, uint32_t value) {
    memcpy(out, &value, sizeof(value));
}
static void store64(void *out, uint64_t value) {
    memcpy(out, &value, sizeof(value));
}
static uint32_t load32(const void *in) {
    uint32_t value;
    memcpy(&value, in, sizeof(value));
    return value;
}

static uint8_t *memory(IM3Runtime runtime, void *base, uint32_t offset, uint32_t size) {
    uint32_t length = m3_GetMemorySize(runtime);
    return offset <= length && size <= length - offset ? (uint8_t *)base + offset : NULL;
}

static const char *const environment[] = {
    "PYTHONHOME=/", "PYTHONDONTWRITEBYTECODE=1", "PYTHONNOUSERSITE=1",
};

static const char *vector_entry(WasiSession *session, bool env, unsigned i) {
    if (!env)
        return session->argv[i];
    return i < sizeof(environment) / sizeof(*environment) ? environment[i] : session->pythonpath;
}

static unsigned vector_count(WasiSession *session, bool env) {
    return env ? sizeof(environment) / sizeof(*environment) + 1 : (unsigned)session->argc;
}

#define RAW(name) static m3ApiRawFunction(name)
#define SESSION WasiSession *session = m3_GetUserData(runtime)
#define ARG(n) ((uint32_t)_sp[(n) + 1])
#define ARG64(n) (_sp[(n) + 1])
#define MEM(offset, size) memory(runtime, _mem, offset, size)
#define RETURN(value) do { _sp[0] = (value); return NULL; } while (0)

static uint32_t vector_size(WasiSession *session, bool env, unsigned count) {
    uint32_t size = 0;
    for (unsigned i = 0; i < count; ++i)
        size += strlen(vector_entry(session, env, i)) + 1;
    return size;
}

RAW(vector_sizes) {
    SESSION;
    bool env = _ctx->userdata != NULL;
    unsigned count = vector_count(session, env);
    uint8_t *n = MEM(ARG(0), 4), *len = MEM(ARG(1), 4);
    if (!n || !len)
        RETURN(WASI_FAULT);
    store32(n, count);
    store32(len, vector_size(session, env, count));
    RETURN(WASI_OK);
}

RAW(vector_get) {
    SESSION;
    bool env = _ctx->userdata != NULL;
    unsigned count = vector_count(session, env);
    uint32_t start = ARG(1);
    uint8_t *array = MEM(ARG(0), count * 4), *strings = MEM(start, vector_size(session, env, count));
    if (!array || !strings)
        RETURN(WASI_FAULT);
    for (unsigned i = 0; i < count; ++i) {
        const char *s = vector_entry(session, env, i);
        size_t len = strlen(s) + 1;
        store32(array + i * 4, start);
        memcpy(strings, s, len);
        strings += len;
        start += len;
    }
    RETURN(WASI_OK);
}

static unsigned file_type(const WasiSession *session, const char *path, uint32_t *size) {
    *size = 0;
    if (!strcmp(path, CPYTHON_STDLIB)) {
        *size = session->stdlib_bytes;
        return TYPE_REGULAR;
    }
    if (!strcmp(path, "/lib"))
        return TYPE_DIRECTORY;
    const File *f = fs_find(path);
    if (!f)
        return 0;
    *size = f->size;
    return f->dir ? TYPE_DIRECTORY : TYPE_REGULAR;
}

static unsigned descriptor_type(WasiSession *session, unsigned fd, uint32_t *size) {
    *size = 0;
    if (fd < 3)
        return TYPE_CHARACTER;
    if (fd >= WASI_FDS || !session->descriptors[fd].path[0])
        return 0;
    return file_type(session, session->descriptors[fd].path, size);
}

static unsigned resolve_path(WasiSession *session, unsigned fd, const char *path, size_t len,
                             char result[PATH_CAP]) {
    uint32_t size;
    if (descriptor_type(session, fd, &size) != TYPE_DIRECTORY)
        return WASI_NOTDIR;
    if (!path)
        return WASI_FAULT;
    if (memchr(path, 0, len))
        return WASI_INVAL;
    const char *base = session->descriptors[fd].path;
    char raw[PATH_CAP];
    size_t prefix = !strcmp(base, "/") ? 0 : strlen(base);
    if (!len || prefix + 1 + len > 120)
        return WASI_NAMETOOLONG;
    memcpy(raw, base, prefix);
    raw[prefix++] = '/';
    memcpy(raw + prefix, path, len);
    raw[prefix + len] = 0;
    return fs_path(raw, result) ? WASI_OK : WASI_NAMETOOLONG;
}

static const char *preopen_name(unsigned fd) {
    return fd == 3 ? "/" : NULL;
}

RAW(prestat_get) {
    const char *name = preopen_name(ARG(0));
    uint8_t *out = MEM(ARG(1), 8);
    if (!name)
        RETURN(WASI_BADF);
    if (!out)
        RETURN(WASI_FAULT);
    memset(out, 0, 8);
    store32(out + 4, strlen(name));
    RETURN(WASI_OK);
}

RAW(prestat_name) {
    const char *name = preopen_name(ARG(0));
    uint8_t *out = MEM(ARG(1), ARG(2));
    if (!name)
        RETURN(WASI_BADF);
    if (!out)
        RETURN(WASI_FAULT);
    if (ARG(2) < strlen(name))
        RETURN(WASI_NAMETOOLONG);
    memcpy(out, name, strlen(name));
    RETURN(WASI_OK);
}

RAW(path_open) {
    SESSION;
    char path[PATH_CAP];
    unsigned error = resolve_path(session, ARG(0), (char *)MEM(ARG(2), ARG(3)), ARG(3), path);
    uint8_t *out = MEM(ARG(8), 4);
    if (!out)
        RETURN(WASI_FAULT);
    if (error)
        RETURN(error);
    if ((ARG(4) & ~2u) || (ARG64(5) & (UINT64_C(1) << 6)) || (ARG(7) & 1))
        RETURN(WASI_ROFS);
    uint32_t size;
    unsigned type = file_type(session, path, &size);
    if (!type)
        RETURN(WASI_NOENT);
    if ((ARG(4) & 2) && type != TYPE_DIRECTORY)
        RETURN(WASI_NOTDIR);
    for (unsigned fd = 5; fd < WASI_FDS; ++fd) {
        if (!session->descriptors[fd].path[0]) {
            strcpy(session->descriptors[fd].path, path);
            session->descriptors[fd].position = 0;
            store32(out, fd);
            RETURN(WASI_OK);
        }
    }
    RETURN(WASI_MFILE);
}

RAW(fd_close) {
    SESSION;
    unsigned fd = ARG(0);
    if (fd >= WASI_FDS || (fd >= 5 && !session->descriptors[fd].path[0]))
        RETURN(WASI_BADF);
    if (fd >= 5)
        session->descriptors[fd].path[0] = 0;
    RETURN(WASI_OK);
}

RAW(fdstat) {
    SESSION;
    uint32_t size;
    unsigned fd = ARG(0), type = descriptor_type(session, fd, &size);
    uint8_t *out = MEM(ARG(1), 24);
    if (!type)
        RETURN(WASI_BADF);
    if (!out)
        RETURN(WASI_FAULT);
    memset(out, 0, 24);
    out[0] = type;
    store64(out + 8, UINT64_C(0x3fffffff));
    store64(out + 16, UINT64_C(0x3fffffff));
    RETURN(WASI_OK);
}

static void filestat(uint8_t *out, unsigned type, uint32_t size) {
    memset(out, 0, 64);
    out[16] = type;
    store64(out + 24, 1);
    store64(out + 32, size);
}

RAW(fd_filestat) {
    SESSION;
    uint32_t size;
    unsigned type = descriptor_type(session, ARG(0), &size);
    uint8_t *out = MEM(ARG(1), 64);
    if (!type)
        RETURN(WASI_BADF);
    if (!out)
        RETURN(WASI_FAULT);
    filestat(out, type, size);
    RETURN(WASI_OK);
}

RAW(path_filestat) {
    SESSION;
    char path[PATH_CAP];
    unsigned error = resolve_path(session, ARG(0), (char *)MEM(ARG(2), ARG(3)), ARG(3), path);
    uint8_t *out = MEM(ARG(4), 64);
    if (!out)
        RETURN(WASI_FAULT);
    if (error)
        RETURN(error);
    uint32_t size;
    unsigned type = file_type(session, path, &size);
    if (!type)
        RETURN(WASI_NOENT);
    filestat(out, type, size);
    RETURN(WASI_OK);
}

static const char *dir_entry(const char *dir, unsigned index, unsigned *type) {
    if (!index) {
        if (!strcmp(dir, "/")) {
            *type = TYPE_DIRECTORY;
            return "lib";
        }
        if (!strcmp(dir, "/lib")) {
            *type = TYPE_REGULAR;
            return "python313.zip";
        }
        return NULL;
    }
    const File *file = &files[index - 1];
    if (!file->used)
        return NULL;
    bool root = !strcmp(dir, "/");
    size_t prefix = root ? 0 : strlen(dir);
    if (strncmp(file->path, dir, prefix) || file->path[prefix] != '/')
        return NULL;
    const char *name = file->path + prefix + 1;
    if (!*name || strchr(name, '/') ||
        !strcmp(file->path, "/lib") || !strcmp(file->path, CPYTHON_STDLIB))
        return NULL;
    *type = file->dir ? TYPE_DIRECTORY : TYPE_REGULAR;
    return name;
}

RAW(read_dir) {
    SESSION;
    unsigned fd = ARG(0);
    uint32_t size, capacity = ARG(2);
    unsigned type = descriptor_type(session, fd, &size);
    if (type != TYPE_DIRECTORY)
        RETURN(type ? WASI_NOTDIR : WASI_BADF);
    uint8_t *buffer = MEM(ARG(1), capacity), *out = MEM(ARG(4), 4);
    if (!buffer || !out)
        RETURN(WASI_FAULT);

    uint32_t used = 0;
    for (uint64_t index = ARG64(3); index <= (unsigned)fs_file_capacity && used < capacity; ++index) {
        const char *name = dir_entry(session->descriptors[fd].path, index, &type);
        if (!name)
            continue;
        uint8_t header[24] = {0};
        size_t len = strlen(name);
        store64(header, index + 1);
        store64(header + 8, index + 1);
        store32(header + 16, len);
        header[20] = type;
        size_t header_bytes = sizeof(header) < capacity - used ? sizeof(header) : capacity - used;
        memcpy(buffer + used, header, header_bytes);
        used += header_bytes;
        size_t name_bytes = len < capacity - used ? len : capacity - used;
        memcpy(buffer + used, name, name_bytes);
        used += name_bytes;
    }
    store32(out, used);
    RETURN(WASI_OK);
}

static bool read_bytes(WasiSession *session, const char *path, uint32_t at, void *out, size_t n) {
    if (!strcmp(path, CPYTHON_STDLIB)) {
        memcpy(out, session->stdlib + at, n);
        return true;
    }
    return fs_read_bytes(fs_find(path), at, out, n);
}

RAW(read_vectors) {
    SESSION;
    unsigned fd = ARG(0), count = ARG(2);
    bool positioned = _ctx->userdata != NULL;
    uint32_t size;
    unsigned type = descriptor_type(session, fd, &size);
    if (count > UINT32_MAX / 8)
        RETURN(WASI_INVAL);
    const uint8_t *vectors = MEM(ARG(1), count * 8);
    uint8_t *out = MEM(ARG(positioned ? 4 : 3), 4);
    if (!vectors || !out)
        RETURN(WASI_FAULT);
    if (!type || fd == 1 || fd == 2)
        RETURN(WASI_BADF);
    if (!fd) {
        store32(out, 0);
        RETURN(WASI_OK);
    }
    if (type != TYPE_REGULAR)
        RETURN(WASI_ISDIR);
    WasiFd *desc = &session->descriptors[fd];
    uint64_t at = positioned ? ARG64(3) : desc->position;
    uint32_t total = 0;
    for (unsigned i = 0; i < count && at < size; ++i) {
        uint32_t offset = load32(vectors + i * 8), len = load32(vectors + i * 8 + 4);
        uint8_t *data = MEM(offset, len);
        if (!data)
            RETURN(WASI_FAULT);
        if (len > size - at)
            len = size - at;
        if (!read_bytes(session, desc->path, at, data, len))
            RETURN(WASI_IO);
        total += len;
        at += len;
    }
    if (!positioned)
        desc->position = at;
    store32(out, total);
    RETURN(WASI_OK);
}

RAW(write_vectors) {
    SESSION;
    unsigned fd = ARG(0), count = ARG(2);
    if (fd != 1 && fd != 2)
        RETURN(WASI_ROFS);
    if (count > UINT32_MAX / 8)
        RETURN(WASI_INVAL);
    const uint8_t *vectors = MEM(ARG(1), count * 8);
    uint8_t *out = MEM(ARG(3), 4);
    if (!vectors || !out)
        RETURN(WASI_FAULT);
    uint32_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t offset = load32(vectors + i * 8), len = load32(vectors + i * 8 + 4);
        const uint8_t *data = MEM(offset, len);
        if (!data || len > UINT32_MAX - total)
            RETURN(WASI_FAULT);
        size_t room = session->cap - session->used - 1;
        size_t copy = len < room ? len : room;
        for (size_t j = 0; j < copy; ++j)
            session->out[session->used++] = data[j] ? (char)data[j] : '?';
        session->out[session->used] = 0;
        total += len;
    }
    store32(out, total);
    RETURN(WASI_OK);
}

RAW(seek_file) {
    SESSION;
    unsigned fd = ARG(0);
    uint32_t size;
    unsigned type = descriptor_type(session, fd, &size);
    if (!type)
        RETURN(WASI_BADF);
    if (type != TYPE_REGULAR)
        RETURN(WASI_SPIPE);
    bool tell = _ctx->userdata != NULL;
    uint8_t *out = MEM(ARG(tell ? 1 : 3), 8);
    if (!out)
        RETURN(WASI_FAULT);
    WasiFd *desc = &session->descriptors[fd];
    if (!tell) {
        unsigned whence = ARG(2);
        int64_t offset = (int64_t)ARG64(1);
        if (whence > 2 || offset > UINT32_MAX || offset < -(int64_t)UINT32_MAX)
            RETURN(WASI_INVAL);
        int64_t position = offset + (whence == 1 ? desc->position : whence == 2 ? size : 0);
        if (position < 0 || position > UINT32_MAX)
            RETURN(WASI_INVAL);
        desc->position = position;
    }
    store64(out, desc->position);
    RETURN(WASI_OK);
}

RAW(clock_get) {
    bool resolution = _ctx->userdata != NULL;
    uint8_t *out = MEM(ARG(resolution ? 1 : 2), 8);
    if (!out)
        RETURN(WASI_FAULT);
    if (ARG(0) > 3)
        RETURN(WASI_INVAL);
    uint64_t ms = resolution ? 1 : ARG(0) ? frame_clock : clock_utc_ms();
    store64(out, ms * UINT64_C(1000000));
    RETURN(WASI_OK);
}

RAW(random_get) {
    uint8_t *out = MEM(ARG(0), ARG(1));
    if (!out)
        RETURN(WASI_FAULT);
    RETURN(random_bytes(out, ARG(1)) ? WASI_OK : WASI_IO);
}

RAW(proc_exit) {
    SESSION;
    session->exit_status = (uint8_t)_sp[0];
    return m3Err_trapExit;
}

RAW(unsupported) {
    RETURN((uintptr_t)_ctx->userdata);
}

M3Result cpython_link_wasi(IM3Module module) {
    static const struct {
        const char *name, *signature;
        M3RawCall callback;
        uintptr_t data;
    } imports[] = {
        {"args_get", "i(ii)", vector_get, 0},
        {"args_sizes_get", "i(ii)", vector_sizes, 0},
        {"environ_get", "i(ii)", vector_get, 1},
        {"environ_sizes_get", "i(ii)", vector_sizes, 1},
        {"clock_res_get", "i(ii)", clock_get, 1},
        {"clock_time_get", "i(iIi)", clock_get, 0},
        {"fd_advise", "i(iIIi)", unsupported, WASI_OK},
        {"fd_close", "i(i)", fd_close, 0},
        {"fd_datasync", "i(i)", unsupported, WASI_ROFS},
        {"fd_fdstat_get", "i(ii)", fdstat, 0},
        {"fd_fdstat_set_flags", "i(ii)", unsupported, WASI_NOSYS},
        {"fd_filestat_get", "i(ii)", fd_filestat, 0},
        {"fd_filestat_set_size", "i(iI)", unsupported, WASI_ROFS},
        {"fd_filestat_set_times", "i(iIIi)", unsupported, WASI_ROFS},
        {"fd_pread", "i(iiiIi)", read_vectors, 1},
        {"fd_prestat_get", "i(ii)", prestat_get, 0},
        {"fd_prestat_dir_name", "i(iii)", prestat_name, 0},
        {"fd_pwrite", "i(iiiIi)", unsupported, WASI_ROFS},
        {"fd_read", "i(iiii)", read_vectors, 0},
        {"fd_readdir", "i(iiiIi)", read_dir, 0},
        {"fd_seek", "i(iIii)", seek_file, 0},
        {"fd_sync", "i(i)", unsupported, WASI_ROFS},
        {"fd_tell", "i(ii)", seek_file, 1},
        {"fd_write", "i(iiii)", write_vectors, 0},
        {"path_create_directory", "i(iii)", unsupported, WASI_ROFS},
        {"path_filestat_get", "i(iiiii)", path_filestat, 0},
        {"path_filestat_set_times", "i(iiiiIIi)", unsupported, WASI_ROFS},
        {"path_link", "i(iiiiiii)", unsupported, WASI_ROFS},
        {"path_open", "i(iiiiiIIii)", path_open, 0},
        {"path_readlink", "i(iiiiii)", unsupported, WASI_INVAL},
        {"path_remove_directory", "i(iii)", unsupported, WASI_ROFS},
        {"path_rename", "i(iiiiii)", unsupported, WASI_ROFS},
        {"path_symlink", "i(iiiii)", unsupported, WASI_ROFS},
        {"path_unlink_file", "i(iii)", unsupported, WASI_ROFS},
        {"poll_oneoff", "i(iiii)", unsupported, WASI_NOSYS},
        {"proc_exit", "v(i)", proc_exit, 0},
        {"sched_yield", "i()", unsupported, WASI_OK},
        {"random_get", "i(ii)", random_get, 0},
        {"sock_accept", "i(iii)", unsupported, WASI_NOSYS},
        {"sock_recv", "i(iiiiii)", unsupported, WASI_NOSYS},
        {"sock_send", "i(iiiii)", unsupported, WASI_NOSYS},
        {"sock_shutdown", "i(ii)", unsupported, WASI_NOSYS},
    };
    for (unsigned i = 0; i < sizeof(imports) / sizeof(*imports); ++i) {
        M3Result result = m3_LinkRawFunctionEx(module, "wasi_snapshot_preview1", imports[i].name,
                                              imports[i].signature, imports[i].callback,
                                              (void *)imports[i].data);
        if (result)
            return result;
    }
    return NULL;
}

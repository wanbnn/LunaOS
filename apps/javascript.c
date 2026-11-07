/* ES2025 runtime for LunaOS scripting. This is NOT a DOM/CSS browser engine. */
#include "quickjs.h"
#include "web.h"
#include "install.h"
#include "task.h"
#include "devkit.h"
static uint32_t deadline;
static char *log_buffer;
static size_t log_cap;
static int interrupt_js(JSRuntime *rt, void *opaque) {
    (void)rt;
    (void)opaque;
    return (int32_t)(frame_clock - deadline) >= 0;
}
static JSValue console_log(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(ctx, argv[i]);
        if (s) {
            if (i)
                append(log_buffer, " ", log_cap);
            append(log_buffer, s, log_cap);
            JS_FreeCString(ctx, s);
        }
    }
    append(log_buffer, "\n", log_cap);
    return JS_UNDEFINED;
}
/* Developer scripts receive a deliberately narrow, versioned native API.
 * QuickJS runs as a trusted kernel service: this is an API boundary,
 * not yet a full untrusted-code sandbox. */
static bool sdk_home_path(const char *input, char path[PATH_CAP]) {
    if (!input || !*input || strlen(input) > 100) return false;
    char candidate[PATH_CAP];
    if (input[0] == '/') {
        strcpy(candidate, input);
    } else {
        const char *home = account_home();
        if (strlen(home) + strlen(input) + 2 >= sizeof(candidate)) return false;
        strcpy(candidate, home);
        strcat(candidate, "/");
        strcat(candidate, input);
    }
    if (!fs_path(candidate, path)) return false;
    const char *home = account_home();
    size_t n = strlen(home);
    return !strncmp(path, home, n) && (path[n] == '/' || path[n] == 0);
}
static JSValue sdk_uptime(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    return JS_NewUint32(ctx, frame_clock);
}
static JSValue sdk_read(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.read(path) requires path");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    char path[PATH_CAP];
    bool allowed = sdk_home_path(name, path);
    JS_FreeCString(ctx, name);
    if (!allowed) return JS_ThrowTypeError(ctx, "path must be within your home directory");
    File *f = fs_find(path);
    if (!f || f->dir) return JS_NULL;
    return JS_NewStringLen(ctx, f->data, f->size);
}
static JSValue sdk_write(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 2) return JS_ThrowTypeError(ctx, "fs.write(path, text) requires two arguments");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    char path[PATH_CAP];
    bool allowed = sdk_home_path(name, path);
    JS_FreeCString(ctx, name);
    if (!allowed || !strcmp(path, account_home()))
        return JS_ThrowTypeError(ctx, "path must be a file inside your home directory");
    const char *body = JS_ToCString(ctx, argv[1]);
    if (!body) return JS_EXCEPTION;
    /* Avoid silent truncation or embedded NUL: LunaFS is a text file system. */
    size_t len = strlen(body);
    bool written = len < FILE_CAP && !fs_write(path, body, false);
    JS_FreeCString(ctx, body);
    return JS_NewBool(ctx, written);
}
static JSValue sdk_list(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.list(directory) requires directory");
    const char *input = JS_ToCString(ctx, argv[0]);
    if (!input) return JS_EXCEPTION;
    char path[PATH_CAP];
    bool allowed = sdk_home_path(input, path);
    JS_FreeCString(ctx, input);
    if (!allowed) return JS_ThrowTypeError(ctx, "directory must be within your home");
    File *dir = fs_find(path);
    if (!dir || !dir->dir) return JS_NULL;
    JSValue list = JS_NewArray(ctx);
    size_t n = strlen(path);
    uint32_t index = 0;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!files[i].used || strncmp(files[i].path, path, n))
            continue;
        if (files[i].path[n] != '/' || !files[i].path[n+1] ||
            strchr(files[i].path + n + 1, '/')) continue;
        JS_SetPropertyUint32(ctx, list, index++, JS_NewString(ctx, files[i].path + n + 1));
    }
    return list;
}
static JSValue sdk_device_read(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 1) return JS_ThrowTypeError(ctx, "device.read(name) requires name");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    char value[128];
    int rc = ldev_read(name, value, sizeof(value));
    JS_FreeCString(ctx, name);
    return rc ? JS_NULL : JS_NewString(ctx, value);
}
static JSValue sdk_device_write(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 2) return JS_ThrowTypeError(ctx, "device.write(name, value) requires arguments");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!value) { JS_FreeCString(ctx, name); return JS_EXCEPTION; }
    int rc = ldev_write(name, value);
    JS_FreeCString(ctx, name); JS_FreeCString(ctx, value);
    return JS_NewBool(ctx, rc == 0);
}
static JSValue sdk_open_app(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc < 1) return JS_ThrowTypeError(ctx, "app.open(name) requires name");
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    static const char *names[] = {"home", "shell", "files", "browser",
                                   "settings", "tasks", "notes"};
    int app = -1;
    for (int i = 0; i < APP_COUNT; i++)
        if (!strcmp(name, names[i])) { app = i; break; }
    JS_FreeCString(ctx, name);
    if (app < 0) return JS_NewBool(ctx, false);
    gui_open(app);
    return JS_NewBool(ctx, true);
}
static void sdk_install(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue luna = JS_NewObject(ctx);
    JSValue fs = JS_NewObject(ctx), dev = JS_NewObject(ctx), app = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, luna, "version", JS_NewString(ctx, VERSION));
    JS_SetPropertyStr(ctx, luna, "apiVersion", JS_NewInt32(ctx, 1));
    JS_SetPropertyStr(ctx, luna, "uptime", JS_NewCFunction(ctx, sdk_uptime, "uptime", 0));
    JS_SetPropertyStr(ctx, fs, "read", JS_NewCFunction(ctx, sdk_read, "read", 1));
    JS_SetPropertyStr(ctx, fs, "write", JS_NewCFunction(ctx, sdk_write, "write", 2));
    JS_SetPropertyStr(ctx, fs, "list", JS_NewCFunction(ctx, sdk_list, "list", 1));
    JS_SetPropertyStr(ctx, luna, "fs", fs);
    JS_SetPropertyStr(ctx, dev, "read", JS_NewCFunction(ctx, sdk_device_read, "read", 1));
    JS_SetPropertyStr(ctx, dev, "write", JS_NewCFunction(ctx, sdk_device_write, "write", 2));
    JS_SetPropertyStr(ctx, luna, "device", dev);
    JS_SetPropertyStr(ctx, app, "open", JS_NewCFunction(ctx, sdk_open_app, "open", 1));
    JS_SetPropertyStr(ctx, luna, "app", app);
    JS_SetPropertyStr(ctx, global, "luna", luna);
    JS_FreeValue(ctx, global);
}
static int eval_internal(const char *source, const char *filename, bool sdk,
                         char *out, size_t cap) {
    out[0] = 0;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) {
        append(out, "JavaScript: out of memory.\n", cap);
        return -1;
    }
    /* Runtime allocations use the growable physical-memory-backed heap. */
    JS_SetMaxStackSize(rt, 48 * 1024);
    JS_SetMemoryLimit(rt, 4 * 1024 * 1024);
    deadline = frame_clock + 100;
    JS_SetInterruptHandler(rt, interrupt_js, 0);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        append(out, "JavaScript: context initialization failed.\n", cap);
        return -1;
    }
    log_buffer = out;
    log_cap = cap;
    JSValue global = JS_GetGlobalObject(ctx), console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, console_log, "log", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
    if (sdk)
        sdk_install(ctx);
    JSValue value = JS_Eval(ctx, source, strlen(source), filename, JS_EVAL_TYPE_GLOBAL);
    bool failed = JS_IsException(value);
    if (failed) {
        JSValue error = JS_GetException(ctx);
        const char *s = JS_ToCString(ctx, error);
        append(out, s ? s : "JavaScript exception", cap);
        append(out, "\n", cap);
        if (s)
            JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, error);
    } else if (!JS_IsUndefined(value)) {
        const char *s = JS_ToCString(ctx, value);
        if (s) {
            append(out, s, cap);
            append(out, "\n", cap);
            JS_FreeCString(ctx, s);
        }
    }
    JS_FreeValue(ctx, value);
    JSContext *jobctx;
    int jobs = 0;
    while (!failed && JS_IsJobPending(rt) && jobs++ < 100) {
        if (JS_ExecutePendingJob(rt, &jobctx) < 0) {
            failed = true;
            append(out, "JavaScript job failed.\n", cap);
        }
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    log_buffer = 0;
    return failed ? -1 : 0;
}

int javascript_eval(const char *source, char *out, size_t cap) {
    return eval_internal(source, "<lunaSH>", false, out, cap);
}
int javascript_eval_sdk(const char *source, const char *filename, char *out, size_t cap) {
    return eval_internal(source, filename, true, out, cap);
}

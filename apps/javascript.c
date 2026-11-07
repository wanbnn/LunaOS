/* ES2025 runtime for LunaOS scripting. This is NOT a DOM/CSS browser engine. */
#include "quickjs.h"
#include "web.h"
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
int javascript_eval(const char *source, char *out, size_t cap) {
    out[0] = 0;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) {
        append(out, "JavaScript: out of memory.\n", cap);
        return -1;
    }
    JS_SetMemoryLimit(rt, 4 * 1024 * 1024);
    JS_SetMaxStackSize(rt, 48 * 1024);
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
    JSValue value = JS_Eval(ctx, source, strlen(source), "<lunaSH>", JS_EVAL_TYPE_GLOBAL);
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

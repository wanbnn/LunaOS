/* LunaPy preview: deliberately small Python-style command interpreter.
 * Real .py files can inspect PCI and call whitelisted device/audio endpoints.
 * NOT CPython/MicroPython: no imports, functions, variables or arbitrary syntax. */
#include "luna.h"
#include "devkit.h"
#include "lunasdk_hardware.h"
#include <stdint.h>

static void skip_spaces(const char **p) {
    while (**p == ' ' || **p == '\t') (*p)++;
}
static bool starts(const char *s, const char *prefix) {
    return !strncmp(s, prefix, strlen(prefix));
}
static void decimal(char out[16], uint32_t value) {
    char digits[16]; unsigned i = 0, j = 0;
    do { digits[i++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (i) out[j++] = digits[--i];
    out[j] = 0;
}
static void add_number(char *out, size_t cap, uint32_t v) {
    char n[16];
    decimal(n, v);
    append(out, n, cap);
}
static int number_arg(const char **cursor, uint32_t *value) {
    const char *p = *cursor;
    skip_spaces(&p);
    unsigned base = 10, digits = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    uint32_t n = 0;
    while (*p) {
        char c = *p;
        int d = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0 || (unsigned)d >= base) break;
        if (n > (UINT32_MAX - (unsigned)d) / base) return -1;
        n = n * base + (unsigned)d;
        p++; digits++;
    }
    if (!digits) return -1;
    skip_spaces(&p);
    *cursor = p;
    *value = n;
    return 0;
}
static int numbers(const char *args, uint32_t *dest, unsigned count) {
    const char *p = args;
    for (unsigned i = 0; i < count; i++) {
        if (number_arg(&p, &dest[i])) return -1;
        if (i + 1 < count) {
            if (*p++ != ',') return -1;
        }
    }
    return *p ? -1 : 0;
}
static int string_arg(const char **cursor, char *out, size_t cap) {
    const char *p = *cursor;
    skip_spaces(&p);
    char quote = *p;
    if (quote != '\'' && quote != '"') return -1;
    p++;
    size_t n = 0;
    while (*p && *p != quote) {
        if (n + 1 >= cap || *p == '\\') return -1;
        out[n++] = *p++;
    }
    if (*p++ != quote) return -1;
    out[n] = 0;
    skip_spaces(&p);
    *cursor = p;
    return 0;
}
static int call_args(const char *input, const char *prefix, char *args, size_t cap) {
    if (!starts(input, prefix)) return -1;
    const char *p = input + strlen(prefix);
    size_t n = strlen(p);
    if (!n || p[n - 1] != ')' || n > cap) return -1;
    memcpy(args, p, n - 1);
    args[n - 1] = 0;
    return 0;
}
static int evaluate(const char *expr, char *out, size_t cap) {
    out[0] = 0;
    const char *p = expr;
    skip_spaces(&p);
    size_t len = strlen(p);
    if (len > 1 && (*p == '\'' || *p == '"')) {
        char value[400];
        if (string_arg(&p, value, sizeof(value)) || *p) return -1;
        append(out, value, cap); return 0;
    }
    char args[400];
    if (!strcmp(p, "luna.hardware.pci.count()")) {
        unsigned count;
        if (lunasdk_pci_count(&count)) return -1;
        add_number(out, cap, count); return 0;
    }
    if (!call_args(p, "luna.hardware.pci.get(", args, sizeof(args))) {
        uint32_t index;
        if (numbers(args, &index, 1)) return -1;
        LunaPciInfo d;
        if (lunasdk_pci_get(index, &d)) { append(out, "None", cap); return 0; }
        append(out, "PCI ", cap);
        add_number(out, cap, d.bus); append(out, ":", cap);
        add_number(out, cap, d.slot); append(out, ".", cap);
        add_number(out, cap, d.function); append(out, " vendor=", cap);
        add_number(out, cap, d.vendor); append(out, " device=", cap);
        add_number(out, cap, d.device); append(out, " class=", cap);
        add_number(out, cap, d.class_code); return 0;
    }
    if (!call_args(p, "luna.hardware.pci.read(", args, sizeof(args))) {
        uint32_t v[5], value;
        if (numbers(args, v, 5) ||
            lunasdk_pci_read(v[0],v[1],v[2],v[3],v[4],&value)) return -1;
        add_number(out, cap, value); return 0;
    }
    if (!call_args(p, "luna.device.read(", args, sizeof(args))) {
        char name[32]; const char *q = args;
        if (string_arg(&q, name, sizeof(name)) || *q) return -1;
        if (ldev_read(name, out, cap)) append(out, "None", cap);
        return 0;
    }
    if (!call_args(p, "luna.device.write(", args, sizeof(args))) {
        char name[32], value[128]; const char *q = args;
        if (string_arg(&q, name, sizeof(name)) || *q++ != ',' ||
            string_arg(&q, value, sizeof(value)) || *q) return -1;
        append(out, ldev_write(name, value) ? "False" : "True", cap);
        return 0;
    }
    if (!call_args(p, "luna.audio.beep(", args, sizeof(args))) {
        uint32_t v[2];
        if (numbers(args, v, 2) || v[0] < 20 || v[0] > 20000 ||
            v[1] < 1 || v[1] > 250) return -1;
        audio_beep(v[0], v[1]);
        append(out, "None", cap); return 0;
    }
    return -1;
}
int lunapy_eval_sdk(const char *source, char *out, size_t cap) {
    if (!source || !out || cap < 4) return -1;
    out[0] = 0;
    unsigned line = 0;
    for (const char *p = source; *p;) {
        if (++line > 128) { append(out, "LunaPy: line limit exceeded\n", cap); return -1; }
        char statement[400]; size_t n = 0;
        while (*p && *p != '\n') {
            if (n + 1 >= sizeof(statement)) {
                append(out, "LunaPy: line too long\n", cap); return -1;
            }
            if (*p != '\r') statement[n++] = *p;
            p++;
        }
        if (*p == '\n') p++;
        statement[n] = 0;
        const char *s = statement;
        skip_spaces(&s);
        if (!*s || *s == '#') continue;
        n = strlen(s);
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
        char compact[400];
        memcpy(compact, s, n);
        compact[n] = 0;
        s = compact;
        bool print_value = starts(s, "print(");
        char value[512];
        if (print_value) {
            size_t k = strlen(s);
            if (k < 7 || s[k - 1] != ')') goto unsupported;
            char expr[400];
            memcpy(expr, s + 6, k - 7);
            expr[k - 7] = 0;
            if (evaluate(expr, value, sizeof(value))) goto unsupported;
            append(out, value, cap);
            append(out, "\n", cap);
        } else if (evaluate(s, value, sizeof(value))) goto unsupported;
        continue;
unsupported:
        append(out, "LunaPy preview: unsupported syntax on line ", cap);
        add_number(out, cap, line);
        append(out, " (not full Python)\n", cap);
        return -1;
    }
    return 0;
}

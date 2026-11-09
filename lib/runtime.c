#include "clock.h"
/* Freestanding ISO-C hooks for the embedded JS engine. No host/Linux syscalls. */
#include "luna.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "dtoa.h"
#include "heap.h"
static int native_errno;
/* Both public and libc-internal callers use the kernel's errno storage. */
int *__errno_location(void) { return &native_errno; }
int *___errno_location(void) __attribute__((alias("__errno_location")));
void *malloc(size_t n) { return heap_allocate(n); }
size_t malloc_usable_size(void *p) { return heap_usable_size(p); }
void free(void *p) { if (heap_release(p)) kernel_fault(); }
void *realloc(void *p, size_t n) { return heap_resize(p,n); }
void *calloc(size_t count, size_t n) {
    if (n && count > 0xffffffffu / n)
        return 0;
    size_t size = count * n;
    void *p = malloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}
_Noreturn void abort(void) {
    serial_write("Native runtime assertion failed\n");
    kernel_fault();
    for (;;)
        ;
}
void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    (void)file;
    (void)line;
    (void)func;
    serial_write(expr);
    abort();
}
struct Output {
    char *data;
    size_t cap, n;
};
static void emit(struct Output *o, char c) {
    if (o->n + 1 < o->cap)
        o->data[o->n] = c;
    o->n++;
}
int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    struct Output o = {out, cap, 0};
    while (*fmt) {
        if (*fmt != '%') {
            emit(&o, *fmt++);
            continue;
        }
        fmt++;
        bool left = false, zero = false, plus = false;
        while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ' || *fmt == '#') {
            left |= *fmt == '-';
            zero |= *fmt == '0';
            plus |= *fmt == '+';
            fmt++;
        }
        int width = 0, prec = -1;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
            if (width < 0) {
                left = true;
                width = -width;
            }
        } else
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + *fmt++ - '0';
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + *fmt++ - '0';
        }
        int length = 0;
        if (*fmt == 'l') {
            fmt++;
            length = 1;
            if (*fmt == 'l') {
                fmt++;
                length = 2;
            }
        } else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') {
            length = *fmt == 'j' ? 2 : 1;
            fmt++;
        }
        char kind = *fmt ? *fmt++ : 0;
        char buf[512];
        const char *s = buf;
        buf[0] = 0;
        if (kind == 's') {
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
        } else if (kind == 'c') {
            buf[0] = va_arg(ap, int);
            buf[1] = 0;
        } else if (kind == '%') {
            buf[0] = '%';
            buf[1] = 0;
        } else if (kind == 'f' || kind == 'g' || kind == 'e' || kind == 'E' || kind == 'G') {
            double d = va_arg(ap, double);
            JSDTOATempMem temp;
            int flags = kind == 'f'                  ? JS_DTOA_FORMAT_FRAC
                        : kind == 'g' || kind == 'G' ? JS_DTOA_FORMAT_FIXED
                                                     : JS_DTOA_FORMAT_FIXED | JS_DTOA_EXP_ENABLED;
            int digits = prec < 0 ? 6 : prec;
            if (digits > 100)
                digits = 100;
            int n = js_dtoa(buf, d, 10, digits, flags, &temp);
            buf[n] = 0;
        } else if (kind == 'd' || kind == 'i' || kind == 'u' || kind == 'x' || kind == 'X' ||
                   kind == 'o' || kind == 'p') {
            uint64_t value;
            bool neg = false;
            unsigned base = kind == 'x' || kind == 'X' || kind == 'p' ? 16 : kind == 'o' ? 8 : 10;
            if (kind == 'p')
                value = (uint32_t)va_arg(ap, void *);
            else if (kind == 'd' || kind == 'i') {
                int64_t v = length == 2   ? va_arg(ap, long long)
                            : length == 1 ? va_arg(ap, long)
                                          : va_arg(ap, int);
                neg = v < 0;
                value = neg ? -(uint64_t)v : (uint64_t)v;
            } else
                value = length == 2   ? va_arg(ap, unsigned long long)
                        : length == 1 ? va_arg(ap, unsigned long)
                                      : va_arg(ap, unsigned);
            char rev[80];
            int n = 0;
            do {
                rev[n++] = "0123456789abcdef"[value % base];
                value /= base;
            } while (value);
            int pos = 0;
            if (neg)
                buf[pos++] = '-';
            else if (plus)
                buf[pos++] = '+';
            while (n)
                buf[pos++] = rev[--n];
            buf[pos] = 0;
        } else {
            buf[0] = kind ? kind : '?';
            buf[1] = 0;
        }
        int n = strlen(s);
        if (kind == 's' && prec >= 0 && n > prec)
            n = prec;
        int pad = width - n;
        if (pad > 4096)
            pad = 4096;
        if (!left)
            while (pad-- > 0)
                emit(&o, zero ? '0' : ' ');
        for (int i = 0; i < n; i++)
            emit(&o, s[i]);
        if (left)
            while (pad-- > 0)
                emit(&o, ' ');
    }
    if (cap)
        out[o.n < cap ? o.n : cap - 1] = 0;
    return o.n;
}
int snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return r;
}
int fprintf(FILE *stream, const char *fmt, ...) {
    (void)stream;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
    return r;
}
int printf(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    serial_write(buf);
    return r;
}
uint32_t rtc_epoch_seconds(void) {
    uint64_t sec=clock_utc_ms()/1000;
    return sec <= UINT32_MAX ? (uint32_t)sec : 0;
}
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    uint64_t ms = clock_utc_ms();
    tv->tv_sec = ms / 1000;
    tv->tv_usec = (ms % 1000) * 1000;
    return 0;
}
struct tm *localtime_r(const time_t *t, struct tm *out) {
    int64_t sec = (int64_t)*t + (int64_t)clock_offset() * 60;
    DateTime d;
    if (sec < 0 || !datetime_from_epoch((uint64_t)sec, &d))
        return NULL;
    memset(out, 0, sizeof(*out));
    out->tm_year = d.year - 1900;
    out->tm_mon = d.month - 1;
    out->tm_mday = d.day;
    out->tm_hour = d.hour;
    out->tm_min = d.minute;
    out->tm_sec = d.second;
    out->tm_wday = (datetime_weekday(&d) + 1) % 7;
    DateTime jan = {d.year, 1, 1, 0, 0, 0};
    uint64_t first;
    datetime_to_epoch(&jan, &first);
    out->tm_yday = ((uint64_t)sec - first) / 86400;
    return out;
}

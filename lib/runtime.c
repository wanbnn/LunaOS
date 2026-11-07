/* Freestanding ISO-C hooks for the embedded JS engine. No host/Linux syscalls. */
#include "luna.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "dtoa.h"
typedef struct Block {
    uint32_t bytes, free;
    struct Block *next;
    uint32_t magic;
} Block;
static uint8_t heap[8 * 1024 * 1024] __attribute__((aligned(16)));
static Block *blocks;
static int native_errno;
int *__errno_location(void) {
    return &native_errno;
}
void *malloc(size_t n) {
    if (!n)
        n = 1;
    if (n > sizeof(heap) - sizeof(Block))
        return 0;
    n = (n + 15) & ~15u;
    if (!blocks) {
        blocks = (Block *)heap;
        *blocks = (Block){sizeof(heap) - sizeof(Block), 1, 0, 0x4c484541};
    }
    for (Block *b = blocks; b; b = b->next)
        if (b->free && b->bytes >= n) {
            if (b->bytes >= n + sizeof(Block) + 16) {
                Block *split = (Block *)((uint8_t *)(b + 1) + n);
                *split = (Block){b->bytes - n - sizeof(Block), 1, b->next, 0x4c484541};
                b->next = split;
                b->bytes = n;
            }
            b->free = 0;
            return b + 1;
        }
    return 0;
}
size_t malloc_usable_size(void *p) {
    return p ? ((Block *)p - 1)->bytes : 0;
}
void free(void *p) {
    if (!p)
        return;
    Block *b = (Block *)p - 1;
    if (b->magic != 0x4c484541)
        kernel_fault();
    b->free = 1;
    for (Block *a = blocks; a && a->next;)
        if (a->free && a->next->free) {
            a->bytes += sizeof(Block) + a->next->bytes;
            a->next = a->next->next;
        } else
            a = a->next;
}
void *realloc(void *p, size_t n) {
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return 0;
    }
    size_t old = malloc_usable_size(p);
    if (old >= n)
        return p;
    void *r = malloc(n);
    if (!r)
        return 0;
    memcpy(r, p, old);
    free(p);
    return r;
}
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
static unsigned read_decimal(const char *s, int n) {
    unsigned v = 0;
    while (n--)
        v = v * 10 + *s++ - '0';
    return v;
}
uint32_t rtc_epoch_seconds(void) {
    char date[40];
    rtc_string(date);
    unsigned year = read_decimal(date, 4), month = read_decimal(date + 5, 2),
             day = read_decimal(date + 8, 2), days = 0;
    for (unsigned y = 1970; y < year; y++)
        days += 365 + (y % 4 == 0 && (y % 100 || y % 400 == 0));
    static const unsigned md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (unsigned m = 1; m < month; m++)
        days += md[m - 1] + (m == 2 && year % 4 == 0 && (year % 100 || year % 400 == 0));
    days += day - 1;
    outb(0x70, 0);
    unsigned seconds = inb(0x71);
    outb(0x70, 0xb);
    if (!(inb(0x71) & 4))
        seconds = (seconds >> 4) * 10 + (seconds & 15);
    return days * 86400 + read_decimal(date + 12, 2) * 3600 + read_decimal(date + 15, 2) * 60 +
           seconds;
}
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    tv->tv_sec = rtc_epoch_seconds();
    tv->tv_usec = (frame_clock % 1000) * 1000;
    return 0;
}
struct tm *localtime_r(const time_t *t, struct tm *out) {
    uint32_t value = (uint32_t)*t, days = value / 86400;
    memset(out, 0, sizeof(*out));
    out->tm_sec = value % 60;
    out->tm_min = value / 60 % 60;
    out->tm_hour = value / 3600 % 24;
    out->tm_wday = (days + 4) % 7;
    unsigned y = 1970;
    while (days >= 365u + (y % 4 == 0 && (y % 100 || y % 400 == 0))) {
        days -= 365 + (y % 4 == 0 && (y % 100 || y % 400 == 0));
        y++;
    }
    out->tm_year = y - 1900;
    out->tm_yday = days;
    static const unsigned md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned month = 0;
    while (month < 11) {
        unsigned n = md[month] + (month == 1 && y % 4 == 0 && (y % 100 || y % 400 == 0));
        if (days < n)
            break;
        days -= n;
        month++;
    }
    out->tm_mon = month;
    out->tm_mday = days + 1;
    return out;
}

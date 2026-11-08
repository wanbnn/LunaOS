/* Narrow i386 glibc ABI compatibility for a host-built libstdc++.a.
 * This is an experimental *link shim*, not a supported glibc runtime port.
 * Rebuild libstdc++ against Luna's musl for a production release.
 */
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
/* libstdc++ uses this glibc optimization only for its single-threaded path.
 * The Luna browser is currently serialized; this must change with threading. */
char __libc_single_threaded = 1;
long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}
int __vsnprintf_chk(char *buf, size_t maxlen, int flags, size_t objsize,
                    const char *fmt, va_list args) {
    (void)flags;
    if (objsize < maxlen) abort();
    return vsnprintf(buf, maxlen, fmt, args);
}
int __snprintf_chk(char *buf, size_t maxlen, int flags, size_t objsize,
                   const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = __vsnprintf_chk(buf, maxlen, flags, objsize, fmt, args);
    va_end(args);
    return ret;
}
int __sprintf_chk(char *buf, int flags, size_t objsize, const char *fmt, ...) {
    (void)flags;
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, objsize, fmt, args);
    va_end(args);
    if (ret < 0 || (size_t)ret >= objsize) abort();
    return ret;
}
int __printf_chk(int flags, const char *fmt, ...) {
    (void)flags;
    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);
    return ret;
}

#include "web.h"
bool random_bytes(void *out, size_t n) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(c & (1u << 30)))
        return false;
    uint8_t *p = out;
    while (n) {
        uint32_t value = 0;
        unsigned char ok = 0;
        for (int i = 0; i < 32 && !ok; i++)
            __asm__ volatile("rdrand %0;setc %1" : "=r"(value), "=qm"(ok));
        if (!ok)
            return false;
        for (int k = 0; k < 4 && n; k++, n--)
            *p++ = value >> (k * 8);
    }
    return true;
}

#include "luna.h"
void *memset(void *d, int c, size_t n) {
    void *r = d;
    uint32_t word = (uint8_t)c * 0x01010101u;
    size_t words = n / 4, bytes = n % 4;
    __asm__ volatile("cld; rep stosl" : "+D"(d), "+c"(words) : "a"(word) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(bytes) : "a"(c) : "memory");
    return r;
}
void *memcpy(void *d, const void *s, size_t n) {
    void *r = d;
    size_t words = n / 4, bytes = n % 4;
    __asm__ volatile("cld; rep movsl" : "+D"(d), "+S"(s), "+c"(words)::"memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(bytes)::"memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n) {
    char *a = d;
    const char *b = s;
    if (a < b)
        return memcpy(d, s, n);
    while (n) {
        n--;
        a[n] = b[n];
    }
    return d;
}
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strcpy(char *d, const char *s) {
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}
char *strcat(char *d, const char *s) {
    strcpy(d + strlen(d), s);
    return d;
}
char *strchr(const char *s, int c) {
    do {
        if (*s == c)
            return (char *)s;
    } while (*s++);
    return 0;
}
int atoi(const char *s) {
    int n = 0, neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        n = n * 10 + *s++ - '0';
    return neg ? -n : n;
}
void number(char *s, uint32_t n) {
    char b[16];
    int i = 0;
    do {
        b[i++] = '0' + n % 10;
        n /= 10;
    } while (n);
    int j = 0;
    while (i)
        s[j++] = b[--i];
    s[j] = 0;
}
void hex(char *s, uint32_t n) {
    strcpy(s, "0x");
    for (int i = 0; i < 8; i++)
        s[i + 2] = "0123456789abcdef"[(n >> ((7 - i) * 4)) & 15];
    s[10] = 0;
}
void append(char *d, const char *s, size_t cap) {
    size_t n = strlen(d);
    while (*s && n + 1 < cap)
        d[n++] = *s++;
    d[n] = 0;
}

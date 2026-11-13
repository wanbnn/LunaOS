/* Temporary deterministic allocator for isolated zlib smoke binaries.
 * Not a production malloc: no reuse, no threads and no heap growth. */
#include <stddef.h>
#include <stdint.h>
static unsigned char area[512u * 1024u] __attribute__((aligned(16)));
static size_t cursor;
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *out=d; const unsigned char *in=s;
    for(size_t i=0;i<n;i++) out[i]=in[i];
    return d;
}
void *memset(void *d, int v, size_t n) {
    unsigned char *out=d;
    for(size_t i=0;i<n;i++) out[i]=(unsigned char)v;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x=a,*y=b;
    for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return (int)x[i]-(int)y[i];
    return 0;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *out=d; const unsigned char *in=s;
    if (out < in) for(size_t i=0;i<n;i++) out[i]=in[i];
    else if (out > in) for(size_t i=n;i>0;i--) out[i-1]=in[i-1];
    return d;
}
void *malloc(size_t n) {
    if (!n) n=1;
    if (n > sizeof(area) || cursor > sizeof(area)-n-15u) return 0;
    size_t at=(cursor + 15u) & ~(size_t)15u;
    if (n>sizeof(area)-at) return 0;
    cursor=at+n; return area+at;
}
void *calloc(size_t count, size_t bytes) {
    if (bytes && count > SIZE_MAX/bytes) return 0;
    size_t n=count*bytes;
    void *p=malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void free(void *p) { (void)p; }

/* Tiny freestanding process-local heap for LunaOS i386.
 * Memory comes from the real guest brk syscall (PMM-backed pages).
 * free() reuses and coalesces blocks. This is a single-threaded bootstrap
 * allocator, NOT upstream musl malloc or a complete POSIX libc. */
#include "posix.h"
#include <stddef.h>
#include <stdint.h>

typedef struct Block Block;
struct Block {
    size_t size;
    Block *next, *prev;
    uint32_t free;
};
static Block *head, *tail;
#define ALIGN16(x) (((x) + 15u) & ~(size_t)15u)
static size_t header_bytes(void) { return ALIGN16(sizeof(Block)); }
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a=d; const unsigned char *b=s;
    for(size_t i=0;i<n;i++) a[i]=b[i];
    return d;
}
void *memset(void *d, int v, size_t n) {
    unsigned char *a=d;
    for(size_t i=0;i<n;i++) a[i]=(unsigned char)v;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x=a,*y=b;
    for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return x[i]-y[i];
    return 0;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *a=d; const unsigned char *b=s;
    if(a<b) for(size_t i=0;i<n;i++)a[i]=b[i];
    else if(a>b) for(size_t i=n;i>0;i--)a[i-1]=b[i-1];
    return d;
}
static void split(Block *block, size_t n) {
    size_t overhead=header_bytes();
    if(block->size < n+overhead+16u) return;
    Block *rest=(Block *)((unsigned char *)block+overhead+n);
    rest->size=block->size-n-overhead;
    rest->next=block->next; rest->prev=block;
    rest->free=1;
    if(rest->next) rest->next->prev=rest;
    else tail=rest;
    block->next=rest; block->size=n;
}
void *malloc(size_t request) {
    if (!request) request=1;
    if(request > SIZE_MAX-32u) return (void *)0;
    size_t n=ALIGN16(request), overhead=header_bytes();
    for(Block *p=head;p;p=p->next)
        if(p->free && p->size>=n) {
            split(p,n);
            p->free=0;
            return (unsigned char *)p+overhead;
        }
    void *start=luna_brk((void *)0);
    if(!start || (uintptr_t)start > UINT32_MAX-n-overhead-16u) return (void *)0;
    /* The heap's first program break is page-aligned. Later extents stay
     * aligned because all block sizes are multiples of 16. */
    uintptr_t end=(uintptr_t)start + overhead+n;
    if(luna_brk((void *)end)!=(void *)end) return (void *)0;
    Block *block=(Block *)start;
    block->size=n; block->free=0; block->next=(void *)0; block->prev=tail;
    if(tail)tail->next=block;
    else head=block;
    tail=block;
    return (unsigned char *)block+overhead;
}
void free(void *ptr) {
    if(!ptr) return;
    Block *b=(Block *)((unsigned char *)ptr-header_bytes());
    b->free=1;
    if(b->next && b->next->free) {
        Block *next=b->next;
        b->size+=header_bytes()+next->size;
        b->next=next->next;
        if(b->next)b->next->prev=b;
        else tail=b;
    }
    if(b->prev && b->prev->free) {
        Block *prev=b->prev;
        prev->size+=header_bytes()+b->size;
        prev->next=b->next;
        if(prev->next)prev->next->prev=prev;
        else tail=prev;
    }
}
void *calloc(size_t count,size_t size) {
    if(size && count>SIZE_MAX/size)return (void *)0;
    size_t n=count*size;
    void *p=malloc(n);
    if(p)memset(p,0,n);
    return p;
}
void *realloc(void *ptr,size_t size) {
    if(!ptr)return malloc(size);
    if(!size){free(ptr);return (void *)0;}
    Block *b=(Block *)((unsigned char *)ptr-header_bytes());
    if(size<=b->size)return ptr;
    void *next=malloc(size);
    if(!next)return (void *)0;
    memcpy(next,ptr,b->size);
    free(ptr);
    return next;
}

/* Minimal freestanding C++ ABI for LunaOS, no host C++ runtime.
 * The STL implementation required by litehtml is a SEPARATE dependency.
 * This supplies allocation / static-local guards / pure-virtual panic only.
 * Build with -fno-exceptions -fno-rtti and do not use throwing C++ libraries.
 */
#include <stddef.h>
#include <stdint.h>
extern "C" void *malloc(size_t);
extern "C" void free(void *);
extern "C" void abort(void);
extern "C" void serial_write(const char *);
static void *alloc_or_abort(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) abort();
    return p;
}
void *operator new(size_t n) { return alloc_or_abort(n); }
void *operator new[](size_t n) { return alloc_or_abort(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
extern "C" {
void __cxa_pure_virtual(void) {
    serial_write("LunaOS C++: pure virtual dispatch\n");
    abort();
}
int __cxa_guard_acquire(uint64_t *guard) {
    /* Luna's current C++ bridge only initializes static locals on one thread.
     * A future parallel renderer must use an atomic guard implementation. */
    return !(*(volatile unsigned char *)guard);
}
void __cxa_guard_release(uint64_t *guard) {
    *(volatile unsigned char *)guard = 1;
}
void __cxa_guard_abort(uint64_t *) {}
/* No dynamic C++ destructors are run on kernel shutdown. */
int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
void *__dso_handle = &__dso_handle;
}

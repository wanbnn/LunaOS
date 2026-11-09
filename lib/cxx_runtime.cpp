/* Kernel hooks for the statically linked C++ runtime used by litehtml.
 */
#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "../kernel/memory.h"
}
extern "C" void *malloc(size_t);
extern "C" void free(void *);
extern "C" void abort(void);
extern "C" void serial_write(const char *);
extern "C" volatile uint32_t frame_clock;
extern "C" uint32_t task_memory_enter(uint32_t pid);
extern "C" void task_memory_leave(uint32_t previous);
namespace std {
[[noreturn]] void __throw_bad_alloc();
}

static size_t renderer_bytes_left;
static size_t renderer_memory_check;
static uint32_t renderer_deadline;
static bool renderer_active;

extern "C" uint32_t luna_cxx_render_begin(void) {
    renderer_bytes_left = 96 * 1024 * 1024;
    renderer_memory_check = renderer_bytes_left;
    renderer_deadline = frame_clock + 1200;
    renderer_active = true;
    // litehtml interns names in static tables that outlive the browser task.
    return task_memory_enter(1);
}
extern "C" void luna_cxx_render_end(uint32_t previous) {
    renderer_active = false;
    task_memory_leave(previous);
}

static void *allocate(size_t n) {
    if (renderer_active) {
        if (n > renderer_bytes_left) {
            serial_write("Browser: litehtml allocation budget exceeded\n");
            std::__throw_bad_alloc();
        }
        if ((int32_t)(frame_clock - renderer_deadline) >= 0) {
            serial_write("Browser: litehtml render deadline exceeded\n");
            std::__throw_bad_alloc();
        }
        renderer_bytes_left -= n;
        if (renderer_bytes_left <= renderer_memory_check) {
            MemoryStats memory;
            memory_stats(&memory);
            if (memory.free_bytes < n + 1024 * 1024) {
                serial_write("Browser: litehtml yielded to the low-memory reserve\n");
                std::__throw_bad_alloc();
            }
            renderer_memory_check = renderer_bytes_left - (renderer_bytes_left > 65536 ? 65536 : renderer_bytes_left);
        }
    }
    void *p = malloc(n ? n : 1);
    if (!p)
        std::__throw_bad_alloc();
    return p;
}
void *operator new(size_t n) { return allocate(n); }
void *operator new[](size_t n) { return allocate(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }
namespace __gnu_cxx {
void __verbose_terminate_handler() {
    serial_write("LunaOS C++: unhandled exception\n");
    abort();
}
}
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

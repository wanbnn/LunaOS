#include "perf.h"
#include "driver.h"
#include "task.h"
#include "storage.h"

PerfSample perf_samples[PERF_COUNT];
static uint64_t started;

static uint64_t cycle_counter(void) {
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}

void perf_pause(void) {
    PerfArea area = task_current()->perf_area;
    if (area)
        perf_samples[area].cycles += cycle_counter() - started;
}

void perf_resume(void) {
    if (task_current()->perf_area)
        started = cycle_counter();
}

PerfArea perf_enter(PerfArea area) {
    Task *task = task_current();
    PerfArea previous = task->perf_area;
    perf_pause();
    task->perf_area = area;
    if (area)
        perf_samples[area].calls++;
    perf_resume();
    return previous;
}

void perf_leave(PerfArea previous) {
    perf_pause();
    task_current()->perf_area = previous;
    perf_resume();
}

void perf_status(char *out, size_t capacity) {
    static const char *const labels[PERF_COUNT] = {
        [PERF_INPUT] = "input", [PERF_DRIVERS] = "drivers", [PERF_NETWORK] = "network poll",
        [PERF_COMPOSE] = "composition", [PERF_SCANOUT] = "scanout", [PERF_CURSOR] = "cursor",
        [PERF_WEB_LOAD] = "web load", [PERF_WEB_LAYOUT] = "web layout", [PERF_WEB_PAINT] = "web paint",
        [PERF_STORAGE] = "storage"
    };
    uint32_t rate = drv_cycles_per_us();
    out[0] = 0;
    append(out, "Subsystem totals since boot (exclusive; sleep excluded)\n", capacity);
    for (unsigned i = 1; i < PERF_COUNT; i++) {
        char number_text[24];
        append(out, labels[i], capacity);
        append(out, "  calls=", capacity);
        number(number_text, perf_samples[i].calls);
        append(out, number_text, capacity);
        if (rate) {
            append(out, "  CPU ms=", capacity);
            uint64_t milliseconds = perf_samples[i].cycles / rate / 1000;
            number(number_text, milliseconds > UINT32_MAX ? UINT32_MAX : (uint32_t)milliseconds);
            append(out, number_text, capacity);
        }
        append(out, "\n", capacity);
    }
    char waits[16];
    number(waits, storage_sleeps);
    append(out, "storage task sleeps=", capacity);
    append(out, waits, capacity);
    append(out, "\n", capacity);
}

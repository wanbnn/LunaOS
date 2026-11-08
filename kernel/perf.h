#ifndef LUNA_PERF_H
#define LUNA_PERF_H
#include "luna.h"

typedef enum {
    PERF_NONE, PERF_INPUT, PERF_DRIVERS, PERF_NETWORK, PERF_COMPOSE, PERF_SCANOUT,
    PERF_CURSOR, PERF_WEB_LOAD, PERF_WEB_LAYOUT, PERF_WEB_PAINT, PERF_STORAGE, PERF_COUNT
} PerfArea;

typedef struct {
    uint64_t cycles;
    uint32_t calls;
} PerfSample;

extern PerfSample perf_samples[PERF_COUNT];
#if __STDC_HOSTED__
/* Host tests exercise driver logic without a kernel scheduler or CPU accounting. */
static inline PerfArea perf_enter(PerfArea area) {
    (void)area;
    return PERF_NONE;
}
static inline void perf_leave(PerfArea previous) {
    (void)previous;
}
#else
PerfArea perf_enter(PerfArea area);
void perf_leave(PerfArea previous);
#endif
void perf_pause(void);
void perf_resume(void);
void perf_status(char *out, size_t capacity);

#endif

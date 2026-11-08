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
PerfArea perf_enter(PerfArea area);
void perf_leave(PerfArea previous);
void perf_pause(void);
void perf_resume(void);
void perf_status(char *out, size_t capacity);

#endif

#include "motion.h"

typedef struct {
    uint32_t started;
    int from, to;
    uint16_t duration;
    bool live;
} Motion;
static Motion channels[MOTION_CHANNELS];

void motion_begin(int channel, uint32_t now_ms, int from, int to, unsigned duration_ms) {
    if (channel < 0 || channel >= MOTION_CHANNELS)
        return;
    if (duration_ms > 1000)
        duration_ms = 1000;
    channels[channel] = (Motion){now_ms, from, to, (uint16_t)duration_ms,
                                  duration_ms > 0 && from != to};
}
void motion_cancel(int channel) {
    if (channel >= 0 && channel < MOTION_CHANNELS)
        channels[channel].live = false;
}
void motion_reset(void) {
    for (int i = 0; i < MOTION_CHANNELS; i++)
        channels[i].live = false;
}
bool motion_active(int channel, uint32_t now_ms) {
    if (channel < 0 || channel >= MOTION_CHANNELS)
        return false;
    Motion *m = &channels[channel];
    return m->live && (uint32_t)(now_ms - m->started) < m->duration;
}
int motion_value(int channel, uint32_t now_ms) {
    if (channel < 0 || channel >= MOTION_CHANNELS)
        return 0;
    Motion *m = &channels[channel];
    if (!m->live)
        return 0;
    uint32_t elapsed = now_ms - m->started;
    if (elapsed >= m->duration)
        return m->to;
    /* Smoothstep, fixed point Q10; no FPU, allocations or LUT. */
    uint32_t t = elapsed * 1024u / m->duration;
    uint32_t sq = (t * t + 512) / 1024;
    uint32_t eased = (sq * (3072 - 2 * t) + 512) / 1024;
    return m->from + (m->to - m->from) * (int)eased / 1024;
}
unsigned motion_spinner_phase(uint32_t now_ms) {
    return (now_ms / 80u) & 7u;
}

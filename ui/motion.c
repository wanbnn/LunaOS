#include "motion.h"
#include <limits.h>
typedef enum { EASE_SMOOTHSTEP, EASE_OUT_CUBIC, EASE_OUT_BACK } MotionEase;
typedef struct {
    uint32_t started;
    int from, to;
    uint16_t duration;
    MotionEase ease;
    bool live;
} Motion;
static Motion channels[MOTION_CHANNELS];
static void begin_eased(int channel, uint32_t now_ms, int from, int to,
                        unsigned duration_ms, MotionEase ease) {
    if (channel < 0 || channel >= MOTION_CHANNELS)
        return;
    if (duration_ms > 1000)
        duration_ms = 1000;
    channels[channel] = (Motion){.started = now_ms, .from = from, .to = to,
                                 .duration = (uint16_t)duration_ms, .ease = ease,
                                 .live = duration_ms > 0 && from != to};
}
void motion_begin(int channel, uint32_t now_ms, int from, int to, unsigned duration_ms) {
    begin_eased(channel, now_ms, from, to, duration_ms, EASE_SMOOTHSTEP);
}
void motion_begin_recipe(int channel, uint32_t now_ms, int from, int to, MotionRecipe recipe) {
    switch (recipe) {
    case MOTION_RECIPE_WINDOW_ENTER:
        begin_eased(channel, now_ms, from, to, 220, EASE_OUT_BACK);
        break;
    case MOTION_RECIPE_WINDOW_RESTORE:
        begin_eased(channel, now_ms, from, to, 200, EASE_OUT_CUBIC);
        break;
    case MOTION_RECIPE_WINDOW_FOCUS:
        begin_eased(channel, now_ms, from, to, 140, EASE_OUT_CUBIC);
        break;
    case MOTION_RECIPE_PANEL_REVEAL:
        begin_eased(channel, now_ms, from, to, 180, EASE_OUT_CUBIC);
        break;
    case MOTION_RECIPE_MENU_DROPDOWN:
        begin_eased(channel, now_ms, from, to, 160, EASE_OUT_CUBIC);
        break;
    case MOTION_RECIPE_CUSTOM:
    default:
        motion_begin(channel, now_ms, from, to, 180);
        break;
    }
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
    const Motion *m = &channels[channel];
    return m->live && (uint32_t)(now_ms - m->started) < m->duration;
}
int motion_value(int channel, uint32_t now_ms) {
    if (channel < 0 || channel >= MOTION_CHANNELS)
        return 0;
    const Motion *m = &channels[channel];
    if (!m->live)
        return 0;
    uint32_t elapsed = now_ms - m->started;
    if (elapsed >= m->duration)
        return m->to;
    /* Q10 fixed-point easing; no floating point or dynamic allocations. */
    int64_t t = (int64_t)elapsed * 1024 / m->duration;
    int64_t eased;
    if (m->ease == EASE_OUT_CUBIC) {
        int64_t u = 1024 - t;
        eased = 1024 - (u * u * u + 524288) / 1048576;
    } else if (m->ease == EASE_OUT_BACK) {
        int64_t u = t - 1024;
        int64_t u2 = (u * u + 512) / 1024;
        int64_t u3 = u2 * u / 1024;
        eased = 1024 + (2766 * u3 + 1742 * u2) / 1024;
    } else {
        int64_t sq = (t * t + 512) / 1024;
        eased = (sq * (3072 - 2 * t) + 512) / 1024;
    }
    int64_t value = (int64_t)m->from + ((int64_t)m->to - m->from) * eased / 1024;
    if (value > INT_MAX)
        value = INT_MAX;
    if (value < INT_MIN)
        value = INT_MIN;
    return (int)value;
}
unsigned motion_spinner_phase(uint32_t now_ms) {
    return (now_ms / 80u) & 7u;
}

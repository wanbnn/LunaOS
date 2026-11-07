#ifndef LUNA_MOTION_H
#define LUNA_MOTION_H
#include <stdbool.h>
#include <stdint.h>

/* Fixed-size, heap-free tween channels. Values are integer pixels/units.
 * Time is the monotonic 1 kHz kernel clock, never the RTC. */
#define MOTION_CHANNELS 7
void motion_begin(int channel, uint32_t now_ms, int from, int to, unsigned duration_ms);
void motion_cancel(int channel);
void motion_reset(void);
bool motion_active(int channel, uint32_t now_ms);
int motion_value(int channel, uint32_t now_ms);
/* Discrete phase for inexpensive 8-dot progress indicators. */
unsigned motion_spinner_phase(uint32_t now_ms);
#endif

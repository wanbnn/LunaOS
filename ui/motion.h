#ifndef LUNA_MOTION_H
#define LUNA_MOTION_H
#include <stdbool.h>
#include <stdint.h>
/* A fixed channel per app plus two native overlays. No Node/React required. */
#define MOTION_CHANNELS 9
typedef enum {
    MOTION_RECIPE_CUSTOM = 0,
    MOTION_RECIPE_WINDOW_ENTER,
    MOTION_RECIPE_WINDOW_RESTORE,
    MOTION_RECIPE_WINDOW_FOCUS,
    MOTION_RECIPE_PANEL_REVEAL,
    MOTION_RECIPE_MENU_DROPDOWN
} MotionRecipe;
void motion_begin(int channel, uint32_t now_ms, int from, int to, unsigned duration_ms);
void motion_begin_recipe(int channel, uint32_t now_ms, int from, int to, MotionRecipe recipe);
void motion_cancel(int channel);
void motion_reset(void);
bool motion_active(int channel, uint32_t now_ms);
int motion_value(int channel, uint32_t now_ms);
unsigned motion_spinner_phase(uint32_t now_ms);
#endif

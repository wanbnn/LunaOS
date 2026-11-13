#include <assert.h>
#include <stdint.h>
#include "motion.h"
int main(void) {
    motion_reset();
    motion_begin(0, 1000, 32, 0, 160);
    assert(motion_value(0, 1000) == 32);
    assert(motion_active(0, 1000));
    int last = 32;
    for (int ms = 1001; ms <= 1160; ms++) {
        int v = motion_value(0, (uint32_t)ms);
        assert(v <= last && v >= 0);
        last = v;
    }
    assert(!motion_active(0, 1160) && motion_value(0, 1160) == 0);
    motion_cancel(0);
    assert(motion_value(0, 1300) == 0);
    motion_begin(1, UINT32_MAX - 30u, -16, 0, 100);
    assert(motion_active(1, 20));
    assert(!motion_active(1, 100));
    assert(motion_value(1, 100) == 0);
    motion_begin(6, 0, 10, 10, 160);
    assert(!motion_active(6, 1));
    motion_begin(99, 0, 10, 0, 160);
    assert(!motion_active(99, 1));
    assert(motion_spinner_phase(0) == 0);
    assert(motion_spinner_phase(80) == 1);
    assert(motion_spinner_phase(640) == 0);
    for (int recipe = MOTION_RECIPE_CUSTOM; recipe <= MOTION_RECIPE_MENU_DROPDOWN; recipe++) {
        motion_begin_recipe(7, 1000, -30, 0, (MotionRecipe)recipe);
        assert(motion_active(7, 1000));
        assert(motion_value(7, 1000) == -30);
        assert(motion_value(7, 2000) == 0);
        assert(!motion_active(7, 2000));
    }
    motion_begin_recipe(8, 0, 28, 0, MOTION_RECIPE_WINDOW_ENTER);
    assert(motion_value(8, 0) == 28);
    assert(motion_value(8, 150) < 0);
    assert(motion_value(8, 220) == 0);
    motion_begin_recipe(7, UINT32_MAX - 20u, -24, 0, MOTION_RECIPE_PANEL_REVEAL);
    assert(motion_active(7, 5));
    assert(motion_value(7, 200) == 0);
    motion_cancel(7);
    assert(motion_value(7, 200) == 0);
    motion_begin_recipe(8, 500, 30, 0, MOTION_RECIPE_MENU_DROPDOWN);
    int previous = 30;
    for (uint32_t tick = 501; tick < 660; tick++) {
        int current = motion_value(8, tick);
        assert(current >= 0 && current <= previous);
        previous = current;
    }
    motion_reset();
    assert(!motion_active(8, 550));
    return 0;
}

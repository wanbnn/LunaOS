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
    return 0;
}

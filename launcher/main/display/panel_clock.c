#include "display/panel_clock.h"

static bool
runnable(int hz) {
    return hz == PANEL_CLOCK_SLOW_HZ || hz == PANEL_CLOCK_FAST_HZ;
}

void
panel_clock_init(panel_clock_t* p, bool found, int32_t saved_hz, int default_hz) {
    p->system_hz = found && runnable(saved_hz) ? saved_hz : default_hz;
}

bool
panel_clock_set_system(panel_clock_t* p, int hz) {
    if (!runnable(hz)) {
        return false;
    }
    p->system_hz = hz;
    return true;
}

int
panel_clock_system_hz(const panel_clock_t* p) {
    return p->system_hz;
}

int
panel_clock_for_switch(const panel_clock_t* p) {
    return p->system_hz;
}

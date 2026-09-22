#include "input/touch_inject_fsm.h"

void
touch_inject_init(touch_inject_t* inject, int x0, int y0, int x1, int y1, uint32_t ms) {
    *inject = (touch_inject_t){
        .x0 = x0,
        .y0 = y0,
        .x1 = x1,
        .y1 = y1,
        .ms = ms,
        .active = true,
    };
}

bool
touch_inject_step(touch_inject_t* inject, int64_t now_us, int* x, int* y) {
    if (!inject->active) {
        return false;
    }
    if (!inject->started) {
        inject->started_us = now_us;
        inject->started = true;
    }
    const int64_t duration_us = (int64_t)inject->ms * 1000;
    int64_t elapsed_us = now_us - inject->started_us;
    if (elapsed_us >= duration_us) {
        if (inject->endpoint_emitted) {
            inject->active = false;
            return false;
        }
        elapsed_us = duration_us;
        inject->endpoint_emitted = true;
    }
    *x = inject->x0 + (int)(((int64_t)(inject->x1 - inject->x0) * elapsed_us) / duration_us);
    *y = inject->y0 + (int)(((int64_t)(inject->y1 - inject->y0) * elapsed_us) / duration_us);
    if (*x == inject->x1 && *y == inject->y1) {
        inject->endpoint_emitted = true;
    }
    return true;
}

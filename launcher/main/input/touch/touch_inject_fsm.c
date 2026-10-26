#include "input/touch/touch_inject_fsm.h"

#include <stddef.h>

void
touch_inject_init(touch_inject_t* inject, vec2i_t from, vec2i_t to, uint32_t ms) {
    *inject = (touch_inject_t){
        .from = from,
        .to = to,
        .ms = ms,
        .active = true,
    };
}

bool
touch_inject_step(touch_inject_t* inject, int64_t now_us, vec2i_t* point) {
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
    point->x = inject->from.x + (int)(((int64_t)(inject->to.x - inject->from.x) * elapsed_us) / duration_us);
    point->y = inject->from.y + (int)(((int64_t)(inject->to.y - inject->from.y) * elapsed_us) / duration_us);
    if (vec2i_equal(*point, inject->to)) {
        inject->endpoint_emitted = true;
    }
    return true;
}

const char*
touch_inject_completion_name(touch_inject_completion_t completion) {
    switch (completion) {
        case TOUCH_INJECT_TAP: return "TAP";
        case TOUCH_INJECT_PRESS: return "PRESS";
        case TOUCH_INJECT_DRAG: return "DRAG";
        case TOUCH_INJECT_NONE: break;
    }
    return NULL;
}

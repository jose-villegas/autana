#include "input/touch/touch_fsm.h"

#include <string.h>

void
touch_fsm_init(touch_fsm_t* fsm) {
    memset(fsm, 0, sizeof(*fsm));
}

void
touch_fsm_update(touch_fsm_t* fsm, bool have_point, bool lifted, vec2i_t point, int64_t now_us) {
    if (have_point) {
        /* The frame needs the old touch's release before it can start a new gesture. */
        if (fsm->released) {
            return;
        }
        if (!fsm->down) {
            /* First contact: latch a press and remember where it began, so a
             * gesture can measure how far the finger has travelled since. */
            fsm->down = true;
            fsm->pressed = true;
            fsm->press = point;
        }
        fsm->point = point;
        fsm->last_contact_us = now_us;
        return;
    }

    if (fsm->down && (lifted || (now_us - fsm->last_contact_us) > TOUCH_RELEASE_QUIET_US)) {
        fsm->down = false;
        fsm->released = true;
    }
}

void
touch_fsm_take(touch_fsm_t* fsm, input_t* out) {
    out->down = fsm->down;
    out->pressed = fsm->pressed;
    out->released = fsm->released;
    out->point = fsm->point;
    out->press = fsm->press;

    fsm->pressed = false;
    fsm->released = false;
}

#include "input/touch/touch_fsm.h"

#include <string.h>

void
touch_fsm_init(touch_fsm_t* fsm) {
    memset(fsm, 0, sizeof(*fsm));
}

void
touch_fsm_update(touch_fsm_t* fsm, bool have_point, vec2i_t point, int64_t now_us) {
    if (have_point) {
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

    /* No contact reported. Only treat that as a lift once the controller has
     * been quiet long enough; a brief gap is a dropout, not a release. */
    if (fsm->down && (now_us - fsm->last_contact_us) > TOUCH_RELEASE_QUIET_US) {
        fsm->down = false;
        fsm->released = true;
    }
}

void
touch_fsm_take(touch_fsm_t* fsm, input_t* out) {
    out->down = fsm->down;
    out->pressed = fsm->pressed;
    out->released = fsm->released;
    out->x = fsm->point.x;
    out->y = fsm->point.y;
    out->press_x = fsm->press.x;
    out->press_y = fsm->press.y;

    fsm->pressed = false;
    fsm->released = false;
}

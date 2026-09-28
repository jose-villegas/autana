#include "input/touch_point.h"

void
touch_point_update_fsm(touch_fsm_t* fsm, bool injection_handled, bool have_point, int* x, int* y, int64_t now_us,
                       bool calibrate, const touch_calib_t* calib, int w, int h) {
    if (have_point) {
        const touch_point_source_t source = injection_handled ? TOUCH_POINT_INJECTED : TOUCH_POINT_CONTROLLER;
        touch_point_for_fsm(source, calibrate, calib, w, h, x, y);
    }
    touch_fsm_update(fsm, have_point, *x, *y, now_us);
}

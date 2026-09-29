#include "input/touch_point.h"

typedef enum {
    TOUCH_POINT_CONTROLLER,
    TOUCH_POINT_INJECTED,
} touch_point_source_t;

void
touch_point_prepare(bool injection_handled, bool have_point, int* x, int* y, bool calibrate, const touch_calib_t* calib,
                    int w, int h) {
    const touch_point_source_t source = injection_handled ? TOUCH_POINT_INJECTED : TOUCH_POINT_CONTROLLER;
    if (have_point && source == TOUCH_POINT_CONTROLLER && calibrate) {
        touch_calib_apply(calib, w, h, x, y);
    }
}

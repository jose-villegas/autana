/* touch_point - prepares a sampled point for the touch state machine. */
#pragma once

#include <stdbool.h>

#include "input/touch_calib.h"

typedef enum {
    TOUCH_POINT_CONTROLLER,
    TOUCH_POINT_INJECTED,
} touch_point_source_t;

static inline void
touch_point_for_fsm(touch_point_source_t source, bool calibrate, const touch_calib_t* calib, int w, int h, int* x,
                    int* y) {
    if (source == TOUCH_POINT_CONTROLLER && calibrate) {
        touch_calib_apply(calib, w, h, x, y);
    }
}

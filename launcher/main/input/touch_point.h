/* touch_point: prepares a sampled point for the touch state machine. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/touch_calib.h"

void touch_point_prepare(bool injection_handled, bool have_point, int* x, int* y, bool calibrate,
                         const touch_calib_t* calib, int w, int h);

/* shake: deliberate shaking from smoothed linear acceleration in screen axes. */
#pragma once

#include <stdint.h>

#include "math/linear/vec3f.h"

/* Mixed-direction and side-to-side captures: carries motion between strokes; docs/input/Shake.md. */
#define SHAKE_TAU_MS  120U
/* Mixed-direction and side-to-side captures: above walking, taps, flips and lifting; docs/input/Shake.md. */
#define SHAKE_START_G 0.5F
/* Mixed-direction and side-to-side captures: hard shaking reaches this size; docs/input/Shake.md. */
#define SHAKE_FULL_G  1.5F

typedef struct {
    float smooth_g;
} shake_t;

void shake_reset(shake_t* s);
/* gravity_linear() in screen axes, in g; per sample or frame, any dt. */
void shake_update(shake_t* s, vec3f_t linear_g, uint32_t dt_us);
/* Zero at/below SHAKE_START_G, linear to one at/above SHAKE_FULL_G. */
float shake_level(const shake_t* s);

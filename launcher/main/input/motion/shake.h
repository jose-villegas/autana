/* shake: deliberate shaking from smoothed linear acceleration in screen axes. */
#pragma once

#include <stdint.h>

#include "math/linear/vec3f.h"

/* Mixed-direction and side-to-side captures: carries motion between strokes; docs/input/Shake.md. */
#define SHAKE_TAU_MS   120U
/* Mixed-direction and side-to-side captures: above walking, taps, flips and lifting; docs/input/Shake.md. */
#define SHAKE_START_MG 500
/* Mixed-direction and side-to-side captures: hard shaking reaches this size; docs/input/Shake.md. */
#define SHAKE_FULL_MG  1500
#define SHAKE_MG_PER_G 1000

typedef struct {
    float smooth_mg;
} shake_t;

void shake_reset(shake_t* s);
/* gravity_linear() in screen axes, in g; per sample or frame, dt capped at GRAVITY_MAX_DT_US. */
void shake_update(shake_t* s, vec3f_t linear_g, uint32_t dt_us);
/* Zero at/below SHAKE_START_MG, linear to one at/above SHAKE_FULL_MG. */
float shake_level(const shake_t* s);

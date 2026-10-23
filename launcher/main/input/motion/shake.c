/* shake: frame-rate-independent deliberate motion intensity. */
#include "input/motion/shake.h"

#include <math.h>

#include "input/motion/gravity.h"

void
shake_reset(shake_t* s) {
    *s = (shake_t){0};
}

void
shake_update(shake_t* s, vec3f_t linear_g, uint32_t dt_us) {
    if (dt_us == 0) {
        return;
    }
    if (dt_us > GRAVITY_MAX_DT_US) {
        dt_us = GRAVITY_MAX_DT_US;
    }
    const float size_mg = sqrtf(vec3f_dot(linear_g, linear_g)) * SHAKE_MG_PER_G;
    const float alpha = -expm1f(-(float)dt_us / (SHAKE_TAU_MS * MATH_US_PER_MS));
    s->smooth_mg += alpha * (size_mg - s->smooth_mg);
}

float
shake_level(const shake_t* s) {
    return mathf_clamp((s->smooth_mg - SHAKE_START_MG) / (SHAKE_FULL_MG - SHAKE_START_MG), 0.0F, 1.0F);
}

/* shake: frame-rate-independent deliberate motion intensity. */
#include "input/motion/shake.h"

#include <math.h>

#include "math/scalar/mathf.h"
#include "math/scalar/units.h"

void
shake_reset(shake_t* s) {
    *s = (shake_t){0};
}

void
shake_update(shake_t* s, vec3f_t linear_g, uint32_t dt_us) {
    if (dt_us == 0) {
        return;
    }
    const float size_g = sqrtf(vec3f_dot(linear_g, linear_g));
    const float alpha = -expm1f(-(float)dt_us / (SHAKE_TAU_MS * MATH_US_PER_MS));
    s->smooth_g += alpha * (size_g - s->smooth_g);
}

float
shake_level(const shake_t* s) {
    return mathf_clamp((s->smooth_g - SHAKE_START_G) / (SHAKE_FULL_G - SHAKE_START_G), 0.0F, 1.0F);
}

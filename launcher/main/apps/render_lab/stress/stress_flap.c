#include "apps/render_lab/stress/stress_flap.h"

#include <math.h>

float
stress_flap_angle(uint32_t t_ms, int copy) {
    const float phase = STRESS_FLAP_PHASE_STEP * (float)copy;
    const float seconds = (float)t_ms / STRESS_FLAP_MS_PER_SECOND;
    return STRESS_FLAP_AMPLITUDE * sinf(2.0F * MATH_PI * STRESS_FLAP_HZ * seconds + phase) + STRESS_FLAP_BIAS;
}

transformf_t
stress_flap_wing(transformf_t rest, float angle, int side) {
    transformf_rotate(&rest, quatf_from_axis_angle(vec3f_forward(), angle * (float)side));
    return rest;
}

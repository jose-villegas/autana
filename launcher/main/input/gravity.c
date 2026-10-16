#include "input/gravity.h"

#include <float.h>
#include <math.h>

#include "math/linear/math_const.h"
#include "math/linear/vec_convert.h"
#include "math/scalar/units.h"

#define HALF_TURN_DEG      180.0F
#define RADIANS_PER_DEGREE (MATH_PI / HALF_TURN_DEG)

void
gravity_reset(gravity_t* g) {
    *g = (gravity_t){.down = {0.0F, 0.0F, -1.0F}, .distrust_ms = GRAVITY_RECOVER_MS + 1U};
}

void
gravity_update(gravity_t* g, const input_motion_t* m, uint32_t dt_ms) {
    if (dt_ms > GRAVITY_MAX_DT_MS) {
        dt_ms = GRAVITY_MAX_DT_MS;
    }
    const float dt = (float)dt_ms / (float)MATH_MS_PER_S;
    const vec3f_t a = vec3f_scale(vec3f_from_vec3i(m->acceleration), 1.0F / INPUT_ACCEL_ONE_G);
    const vec3f_t raw = vec3f_scale(vec3f_from_vec3i(m->rotation), 1.0F / INPUT_ROTATION_ONE_DPS);
    const vec3f_t rate = vec3f_sub(raw, g->gyro_bias);
    const vec3f_t w = vec3f_scale(rate, RADIANS_PER_DEGREE);
    g->down = vec3f_normalize(vec3f_sub(g->down, vec3f_scale(vec3f_cross(w, g->down), dt)));

    const float magnitude = sqrtf(vec3f_dot(a, a));
    if (magnitude > FLT_EPSILON) {
        const vec3f_t direction = vec3f_scale(a, 1.0F / magnitude);
        const float error = fabsf(magnitude - 1.0F);
        const float weight =
            mathf_clamp((GRAVITY_TRUST_NONE_G - error) / (GRAVITY_TRUST_NONE_G - GRAVITY_TRUST_FULL_G), 0.0F, 1.0F);
        if (g->distrust_ms <= GRAVITY_RECOVER_MS && weight > 0.0F
            && vec3f_dot(direction, g->down) >= GRAVITY_REJECT_COS) {
            /* Mahony et al.: down cross direction learns the bias for our minus-cross prediction.
             * Error is perpendicular to down: along-down bias is unobservable and rotating down
             * about itself changes nothing. Convert the radian correction to bias in deg/s. */
            const vec3f_t correction = vec3f_cross(g->down, direction);
            const vec3f_t bias =
                vec3f_add(g->gyro_bias, vec3f_scale(correction, GRAVITY_BIAS_KI * weight * dt / RADIANS_PER_DEGREE));
            g->gyro_bias = (vec3f_t){mathf_clamp(bias.x, -GRAVITY_BIAS_MAX_DPS, GRAVITY_BIAS_MAX_DPS),
                                     mathf_clamp(bias.y, -GRAVITY_BIAS_MAX_DPS, GRAVITY_BIAS_MAX_DPS),
                                     mathf_clamp(bias.z, -GRAVITY_BIAS_MAX_DPS, GRAVITY_BIAS_MAX_DPS)};
            /* dt/(tau+dt) approximates exponential smoothing without a per-frame expf. */
            const float alpha = weight * dt / (GRAVITY_TAU_S + dt);
            g->down = vec3f_normalize(vec3f_add(g->down, vec3f_scale(vec3f_sub(direction, g->down), alpha)));
            g->distrust_ms = 0;
        } else {
            g->distrust_ms += dt_ms;
            if (g->distrust_ms > GRAVITY_RECOVER_MS) {
                g->down = direction;
                g->distrust_ms = 0;
            }
        }
    }
    g->linear = vec3f_sub(a, g->down);
}

vec3f_t
gravity_down(const gravity_t* g) {
    return g->down;
}

vec3f_t
gravity_linear(const gravity_t* g) {
    return g->linear;
}

float
gravity_strength(const gravity_t* g) {
    if (gravity_in_free_fall(g)) {
        return 0.0F;
    }
    return mathf_clamp(sqrtf(g->down.x * g->down.x + g->down.y * g->down.y), 0.0F, 1.0F);
}

bool
gravity_in_free_fall(const gravity_t* g) {
    const vec3f_t a = vec3f_add(g->linear, g->down);
    return vec3f_dot(a, a) < GRAVITY_FREE_FALL_G * GRAVITY_FREE_FALL_G;
}

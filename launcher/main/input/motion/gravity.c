#include "input/motion/gravity.h"

#include <float.h>
#include <math.h>

#include "math/linear/vec_convert.h"
#include "math/scalar/math_const.h"
#include "math/scalar/units.h"

#define HALF_TURN_DEG      180.0F
#define RADIANS_PER_DEGREE (MATH_PI / HALF_TURN_DEG)

void
gravity_reset(gravity_t* g) {
    *g = (gravity_t){.down = {0.0F, 0.0F, -1.0F}, .primed = false};
}

static vec3f_t
predict(gravity_t* g, vec3f_t raw, float dt) {
    const vec3f_t rate = vec3f_sub(raw, g->gyro_bias);
    const vec3f_t w = vec3f_scale(rate, RADIANS_PER_DEGREE);
    g->down = vec3f_normalize(vec3f_sub(g->down, vec3f_scale(vec3f_cross(w, g->down), dt)));
    return rate;
}

static void
restart(gravity_t* g) {
    g->recovering = true;
    g->distrust_us = GRAVITY_RECOVER_US;
    g->still_us = 0;
}

static void
check_overrange(gravity_t* g, vec3f_t raw) {
    if (fabsf(raw.x) >= GRAVITY_GYRO_OVERRANGE_DPS || fabsf(raw.y) >= GRAVITY_GYRO_OVERRANGE_DPS
        || fabsf(raw.z) >= GRAVITY_GYRO_OVERRANGE_DPS) {
        restart(g);
    }
}

bool
gravity_accel_trusted(float magnitude_g) {
    return fabsf(magnitude_g - 1.0F) <= GRAVITY_TRUST_FULL_G;
}

static float
trust_weight(float error) {
    return mathf_clamp((GRAVITY_TRUST_NONE_G - error) / (GRAVITY_TRUST_NONE_G - GRAVITY_TRUST_FULL_G), 0.0F, 1.0F);
}

static void
track_trust(gravity_t* g, bool accepted, bool still, uint32_t dt_us) {
    if (accepted) {
        /* x-io Fusion, FusionAhrs.c: at fixed rate this equals its -9/+1 counter,
         * with sample limit = rate * GRAVITY_RECOVER_US / MATH_US_PER_S.
         * Time weighting preserves that ratio with variable frame intervals. */
        const uint32_t drain = GRAVITY_RECOVER_ACCEPT_WEIGHT * dt_us;
        g->distrust_us = g->distrust_us > drain ? g->distrust_us - drain : 0;
        g->recovering = false;
        g->still_us = 0;
        return;
    }
    const uint32_t debt = g->distrust_us + dt_us;
    g->recovering |= debt > GRAVITY_RECOVER_US;
    g->distrust_us = debt < GRAVITY_RECOVER_US ? debt : GRAVITY_RECOVER_US;
    g->still_us += still ? dt_us : 0;
}

typedef enum { CORRECT_NONE, CORRECT_ADOPT, CORRECT_FULL_GAIN, CORRECT_BIAS_PULL } correction_mode_t;

static correction_mode_t
select_correction(const gravity_t* g, bool accepted, bool was_recovering) {
    if (!g->primed || g->still_us > GRAVITY_STILL_RECOVER_US) {
        return CORRECT_ADOPT;
    }
    if (accepted && !was_recovering) {
        return CORRECT_BIAS_PULL;
    }
    return g->recovering || was_recovering ? CORRECT_FULL_GAIN : CORRECT_NONE;
}

static void
correct_accepted(gravity_t* g, vec3f_t direction, float weight, float dt) {
    /* Mahony et al.: down cross direction learns the bias for our minus-cross prediction.
     * Error is perpendicular to down: along-down bias is unobservable and rotating down
     * about itself changes nothing. Convert the radian correction to bias in deg/s. */
    const vec3f_t correction = vec3f_cross(g->down, direction);
    const vec3f_t bias =
        vec3f_add(g->gyro_bias, vec3f_scale(correction, GRAVITY_BIAS_KI * weight * dt / RADIANS_PER_DEGREE));
    const vec3f_t hi = {IMU_GYRO_BIAS_MAX_DPS, IMU_GYRO_BIAS_MAX_DPS, IMU_GYRO_BIAS_MAX_DPS};
    g->gyro_bias = vec3f_clamp(bias, vec3f_scale(hi, -1.0F), hi);
    /* dt/(tau+dt) approximates exponential smoothing without a per-frame expf. */
    const float alpha = weight * dt / (GRAVITY_TAU_S + dt);
    g->down = vec3f_normalize(vec3f_lerp(g->down, direction, alpha));
}

static void
correct_acceleration(gravity_t* g, vec3f_t a, vec3f_t rate, uint32_t dt_us, float dt) {
    const float magnitude = sqrtf(vec3f_dot(a, a));
    const float error = fabsf(magnitude - 1.0F);
    const bool still =
        vec3f_dot(rate, rate) < GRAVITY_STILL_DPS * GRAVITY_STILL_DPS && gravity_accel_trusted(magnitude);
    if (!still) {
        g->still_us = 0;
    }
    if (magnitude <= FLT_EPSILON) {
        return;
    }
    const vec3f_t direction = vec3f_scale(a, 1.0F / magnitude);
    const float weight = trust_weight(error);
    const bool accepted = weight > 0.0F && vec3f_dot(direction, g->down) >= GRAVITY_REJECT_COS;
    const bool was_recovering = g->recovering;
    track_trust(g, accepted, still, dt_us);
    const correction_mode_t mode = select_correction(g, accepted, was_recovering);
    if (mode == CORRECT_ADOPT) {
        g->down = direction;
        g->primed = true;
        g->recovering = false;
        g->distrust_us = 0;
        g->still_us = 0;
    } else if (mode == CORRECT_BIAS_PULL) {
        correct_accepted(g, direction, weight, dt);
    } else if (mode == CORRECT_FULL_GAIN) {
        g->down = vec3f_normalize(vec3f_lerp(g->down, direction, dt / (GRAVITY_TAU_S + dt)));
    }
}

void
gravity_update(gravity_t* g, const input_motion_t* m, uint32_t dt_us) {
    if (dt_us > GRAVITY_MAX_DT_US) {
        dt_us = GRAVITY_MAX_DT_US;
    }
    const float dt = (float)dt_us / (float)MATH_US_PER_S;
    const vec3f_t a = vec3f_scale(vec3f_from_vec3i(m->acceleration), 1.0F / INPUT_ACCEL_ONE_G);
    const vec3f_t raw = vec3f_scale(vec3f_from_vec3i(m->rotation), 1.0F / INPUT_ROTATION_ONE_DPS);
    g->rotation = raw;
    const vec3f_t rate = predict(g, raw, dt);
    check_overrange(g, raw);
    correct_acceleration(g, a, rate, dt_us, dt);
    g->linear = vec3f_sub(a, g->down);
}

uint32_t
gravity_held_us(uint8_t lost) {
    const uint32_t lost_us = (uint32_t)lost * INPUT_MOTION_SAMPLE_US;
    return lost_us > GRAVITY_MAX_DT_US ? GRAVITY_MAX_DT_US : lost_us;
}

void
gravity_update_batch(gravity_t* g, const input_motion_batch_t* batch) {
    if (batch->gap) {
        restart(g);
    }
    for (int i = 0; i < batch->count; i++) {
        /* Hold the preceding sample's rotation across read-mode loss, without acceleration correction. */
        if (batch->lost_before[i] > 0) {
            uint32_t remaining = gravity_held_us(batch->lost_before[i]);
            while (remaining > 0) {
                const uint32_t step = remaining < INPUT_MOTION_SAMPLE_US ? remaining : INPUT_MOTION_SAMPLE_US;
                predict(g, g->rotation, (float)step / MATH_US_PER_S);
                remaining -= step;
            }
        }
        gravity_update(g, &batch->samples[i], INPUT_MOTION_SAMPLE_US);
    }
}

vec3f_t
gravity_down(const gravity_t* g) {
    return g->down;
}

vec3f_t
gravity_linear(const gravity_t* g) {
    return g->linear;
}

vec3f_t
gravity_gyro_bias(const gravity_t* g) {
    return g->gyro_bias;
}

float
gravity_strength(const gravity_t* g) {
    if (gravity_in_free_fall(g)) {
        return 0.0F;
    }
    const vec2f_t plane = vec3f_xy(g->down);
    return mathf_clamp(sqrtf(vec2f_dot(plane, plane)), 0.0F, 1.0F);
}

bool
gravity_in_free_fall(const gravity_t* g) {
    const vec3f_t a = vec3f_add(g->linear, g->down);
    return vec3f_dot(a, a) < GRAVITY_FREE_FALL_G * GRAVITY_FREE_FALL_G;
}

void
gravity_clear_gyro_bias(gravity_t* g) {
    g->gyro_bias = (vec3f_t){0};
}

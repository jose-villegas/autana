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
check_overrange(gravity_t* g, vec3f_t raw) {
    if (fabsf(raw.x) >= GRAVITY_GYRO_OVERRANGE_DPS || fabsf(raw.y) >= GRAVITY_GYRO_OVERRANGE_DPS
        || fabsf(raw.z) >= GRAVITY_GYRO_OVERRANGE_DPS) {
        g->recovering = true;
        g->distrust_ms = GRAVITY_RECOVER_MS;
    }
}

static float
trust_weight(float error) {
    return mathf_clamp((GRAVITY_TRUST_NONE_G - error) / (GRAVITY_TRUST_NONE_G - GRAVITY_TRUST_FULL_G), 0.0F, 1.0F);
}

static void
accept_sample(gravity_t* g, uint32_t dt_ms) {
    /* x-io Fusion, FusionAhrs.c: at fixed rate this equals its -9/+1 counter,
     * with sample limit = rate * GRAVITY_RECOVER_MS / MATH_MS_PER_S.
     * Time weighting preserves that ratio with variable frame intervals. */
    const uint32_t drain = GRAVITY_RECOVER_ACCEPT_WEIGHT * dt_ms;
    g->distrust_ms = g->distrust_ms > drain ? g->distrust_ms - drain : 0;
    g->recovering = false;
    g->still_ms = 0;
}

static void
reject_sample(gravity_t* g, bool still, uint32_t dt_ms) {
    const uint32_t debt = g->distrust_ms + dt_ms;
    if (debt > GRAVITY_RECOVER_MS) {
        g->recovering = true;
    }
    g->distrust_ms = debt < GRAVITY_RECOVER_MS ? debt : GRAVITY_RECOVER_MS;
    if (still) {
        g->still_ms += dt_ms;
    }
}

static bool
adopt_if_ready(gravity_t* g, vec3f_t direction) {
    if (g->primed && g->still_ms <= GRAVITY_STILL_RECOVER_MS) {
        return false;
    }
    g->down = direction;
    g->primed = true;
    g->recovering = false;
    g->distrust_ms = 0;
    g->still_ms = 0;
    return true;
}

static void
correct_accepted(gravity_t* g, vec3f_t direction, float weight, float dt) {
    /* Mahony et al.: down cross direction learns the bias for our minus-cross prediction.
     * Error is perpendicular to down: along-down bias is unobservable and rotating down
     * about itself changes nothing. Convert the radian correction to bias in deg/s. */
    const vec3f_t correction = vec3f_cross(g->down, direction);
    const vec3f_t bias =
        vec3f_add(g->gyro_bias, vec3f_scale(correction, GRAVITY_BIAS_KI * weight * dt / RADIANS_PER_DEGREE));
    const vec3f_t hi = {GRAVITY_BIAS_MAX_DPS, GRAVITY_BIAS_MAX_DPS, GRAVITY_BIAS_MAX_DPS};
    g->gyro_bias = vec3f_clamp(bias, vec3f_scale(hi, -1.0F), hi);
    /* dt/(tau+dt) approximates exponential smoothing without a per-frame expf. */
    const float alpha = weight * dt / (GRAVITY_TAU_S + dt);
    g->down = vec3f_normalize(vec3f_lerp(g->down, direction, alpha));
}

static void
correct_acceleration(gravity_t* g, vec3f_t a, vec3f_t rate, uint32_t dt_ms, float dt) {
    const float magnitude = sqrtf(vec3f_dot(a, a));
    const float error = fabsf(magnitude - 1.0F);
    const bool still = vec3f_dot(rate, rate) < GRAVITY_STILL_DPS * GRAVITY_STILL_DPS && error <= GRAVITY_TRUST_FULL_G;
    if (!still) {
        g->still_ms = 0;
    }
    if (magnitude <= FLT_EPSILON) {
        return;
    }
    const vec3f_t direction = vec3f_scale(a, 1.0F / magnitude);
    const float weight = trust_weight(error);
    const bool accepted = weight > 0.0F && vec3f_dot(direction, g->down) >= GRAVITY_REJECT_COS;
    const bool was_recovering = g->recovering;
    if (accepted) {
        accept_sample(g, dt_ms);
    } else {
        reject_sample(g, still, dt_ms);
    }
    if (adopt_if_ready(g, direction)) {
        return;
    }
    if (accepted && !was_recovering) {
        correct_accepted(g, direction, weight, dt);
    } else if (g->recovering || was_recovering) {
        const float alpha = dt / (GRAVITY_TAU_S + dt);
        g->down = vec3f_normalize(vec3f_lerp(g->down, direction, alpha));
    }
}

void
gravity_update(gravity_t* g, const input_motion_t* m, uint32_t dt_ms) {
    if (dt_ms > GRAVITY_MAX_DT_MS) {
        dt_ms = GRAVITY_MAX_DT_MS;
    }
    const float dt = (float)dt_ms / (float)MATH_MS_PER_S;
    const vec3f_t a = vec3f_scale(vec3f_from_vec3i(m->acceleration), 1.0F / INPUT_ACCEL_ONE_G);
    const vec3f_t raw = vec3f_scale(vec3f_from_vec3i(m->rotation), 1.0F / INPUT_ROTATION_ONE_DPS);
    const vec3f_t rate = predict(g, raw, dt);
    check_overrange(g, raw);
    correct_acceleration(g, a, rate, dt_ms, dt);
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

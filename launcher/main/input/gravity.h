/*
 * gravity: gyro-predicted down and linear acceleration in screen axes.
 * Pure logic, no sensor and no clock: motion and elapsed time are passed in.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"
#include "math/linear/vec3f.h"

/* Ours, tuned on the board. */
#define GRAVITY_TAU_S        0.5F
/* Ours: full trust within this departure from one g. */
#define GRAVITY_TRUST_FULL_G 0.1F
/* Android WindowOrientationListener's acceleration tolerance, 4 m/s^2. */
#define GRAVITY_TRUST_NONE_G 0.4F
/* Madgwick, x-io Fusion's default acceleration rejection. */
#define GRAVITY_REJECT_DEG   10.0F
/* Madgwick, x-io Fusion's default recovery trigger period, in milliseconds. */
#define GRAVITY_RECOVER_MS   5000U
/* M0 board capture: gyro noise peaks at +/-4 deg/s. */
#define GRAVITY_STILL_DPS    6.0F
/* Ours: maximum linear acceleration while learning gyro bias. */
#define GRAVITY_STILL_G      0.05F
/* Ours: gyro bias learning time constant. */
#define GRAVITY_BIAS_TAU_S   5.0F
/* From tilt's TILT_FREE_FALL_PCT, in g. */
#define GRAVITY_FREE_FALL_G  0.3F
/* From tilt's TILT_MAX_DT_MS, in milliseconds. */
#define GRAVITY_MAX_DT_MS    100U

typedef struct {
    vec3f_t down;
    vec3f_t linear;
    vec3f_t gyro_bias;
    uint32_t distrust_ms;
} gravity_t;

/* The first nonzero acceleration after reset is adopted exactly. */
void gravity_reset(gravity_t* g);
void gravity_update(gravity_t* g, const input_motion_t* m, uint32_t dt_ms);
vec3f_t gravity_down(const gravity_t* g);
vec3f_t gravity_linear(const gravity_t* g);
/* In-plane share of g, 0..1; zero during free fall. */
float gravity_strength(const gravity_t* g);
bool gravity_in_free_fall(const gravity_t* g);

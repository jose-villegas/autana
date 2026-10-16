/*
 * gravity: estimates down and linear acceleration in screen axes. Gyro rotation
 * predicts down; acceleration only corrects it because an accelerometer cannot
 * distinguish gravity from other acceleration. Pure logic: no sensor or clock.
 *
 * Each update clamps time, predicts down from the bias-corrected gyro, corrects
 * with trusted acceleration (or recovers after rejection), subtracts down to
 * find linear acceleration, and integrates accepted correction error into bias.
 *
 * tilt stays beside it for now.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"
#include "math/linear/vec3f.h"

/* Ours, tuned on the board. */
#define GRAVITY_TAU_S                     0.5F
/* Ours: full trust within this departure from one g. */
#define GRAVITY_TRUST_FULL_G              0.1F
/* Android WindowOrientationListener's acceleration tolerance, 4 m/s^2. */
#define GRAVITY_TRUST_NONE_G              0.4F
/* Madgwick, x-io Fusion's default acceleration rejection. */
#define GRAVITY_REJECT_DEG                10.0F
/* cos(GRAVITY_REJECT_DEG), precomputed with the angle in degrees. */
#define GRAVITY_REJECT_COS                0.984807753F
/* Madgwick, x-io Fusion's default recovery trigger period, in milliseconds. */
#define GRAVITY_RECOVER_MS                5000U
/* QMI8658C datasheet, Table 8: board-level initial gyro offset tolerance. */
#define GRAVITY_GYRO_OFFSET_TOL_DPS       10.0F
/* QMI8658C datasheet, Table 8: gyro offset temperature coefficient. */
#define GRAVITY_GYRO_OFFSET_TCO_DPS_PER_C 0.05F
/* QMI8658C datasheet, Table 8: from the 25 C reference to the 85 C operating limit. */
#define GRAVITY_GYRO_TEMP_SPAN_C          60.0F
/* QMI8658C datasheet, Table 8: initial offset plus temperature drift, in deg/s. */
#define GRAVITY_BIAS_MAX_DPS                                                                                           \
    (GRAVITY_GYRO_OFFSET_TOL_DPS + GRAVITY_GYRO_OFFSET_TCO_DPS_PER_C * GRAVITY_GYRO_TEMP_SPAN_C)
/* Ours: target 2% settling time for a datasheet-sized gyro offset. */
#define GRAVITY_BIAS_SETTLE_S 20.0F
/* Mahony et al.: theta' = b_err - theta/TAU, b_err' = -KI*theta gives
 * s^2 + s/TAU + KI = 0. For KI << 1/(4*TAU^2), the slow pole is about
 * -KI*TAU. Four slow time constants give 2% settling: KI = 4/(TAU*SETTLE).
 * KI is in 1/s^2; at our settings 0.4 < 1/(4*TAU^2), so the loop is overdamped. */
#define GRAVITY_BIAS_KI       (4.0F / (GRAVITY_TAU_S * GRAVITY_BIAS_SETTLE_S))
/* From tilt's TILT_FREE_FALL_PCT, in g. */
#define GRAVITY_FREE_FALL_G   0.3F
/* From tilt's TILT_MAX_DT_MS, in milliseconds. */
#define GRAVITY_MAX_DT_MS     100U

typedef struct {
    /* Unit vector, screen axes: x right, y down, z out. */
    vec3f_t down;
    /* Last acceleration minus down, in g. */
    vec3f_t linear;
    /* Gyro bias in deg/s, learned only from accepted acceleration. */
    vec3f_t gyro_bias;
    /* Accel rejection time; past GRAVITY_RECOVER_MS the next nonzero reading is adopted. */
    uint32_t distrust_ms;
} gravity_t;

/* Reset to down=(0,0,-1), zero linear g and bias deg/s; adopt the first nonzero acceleration exactly. */
void gravity_reset(gravity_t* g);
/* Motion uses INPUT_ACCEL_ONE_G and INPUT_ROTATION_ONE_DPS counts in screen axes.
 * dt_ms is clamped to GRAVITY_MAX_DT_MS; zero dt skips prediction and blending.
 * Zero acceleration cannot correct down; recovery can adopt a reading even at zero dt. */
void gravity_update(gravity_t* g, const input_motion_t* m, uint32_t dt_ms);
/* Unit down vector in screen axes, including during free fall. */
vec3f_t gravity_down(const gravity_t* g);
/* Last acceleration minus down in screen axes, in g; zero after reset. */
vec3f_t gravity_linear(const gravity_t* g);
/* sin of tilt from flat, 0..1; zero during free fall. */
float gravity_strength(const gravity_t* g);
/* True when |linear + down| is below GRAVITY_FREE_FALL_G; false at the boundary. */
bool gravity_in_free_fall(const gravity_t* g);

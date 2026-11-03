/* gravity: estimates down, linear acceleration and gyro bias in screen axes.
 * Acceleration cannot distinguish gravity from other forces, so magnitude
 * trust and angular acceptance gate correction. Pure logic: no sensor or clock. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"
#include "input/motion/imu.h"
#include "input/motion/imu_rest.h"
#include "math/linear/vec3f.h"
#include "math/scalar/units.h"

/* Minimum in-plane strength (0..1) for a meaningful direction. */
#define GRAVITY_MIN_DIRECTION_STRENGTH 0.25F

/* Ours, tuned on the board. */
#define GRAVITY_TAU_S                  0.5F
/* Ours: full trust within this departure from one g. */
#define GRAVITY_TRUST_FULL_G           0.1F
/* Android WindowOrientationListener's acceleration tolerance, 4 m/s^2. */
#define GRAVITY_TRUST_NONE_G           0.4F
/* Madgwick, x-io Fusion's recommended acceleration rejection. */
#define GRAVITY_REJECT_DEG             10.0F
/* cos(GRAVITY_REJECT_DEG), precomputed with the angle in degrees. */
#define GRAVITY_REJECT_COS             0.984807753F
/* x-io Fusion, FusionAhrs.c: recommended rejectionTimeout, in milliseconds. */
#define GRAVITY_RECOVER_MS             5000U
/* x-io Fusion, FusionAhrs.c: accepted samples drain nine rejected samples. */
#define GRAVITY_RECOVER_ACCEPT_WEIGHT  9U
/* x-io Fusion, FusionAhrs.c: restart trust near the configured gyro full scale. */
#define GRAVITY_GYRO_OVERRANGE_RATIO   0.98F
#define GRAVITY_GYRO_OVERRANGE_DPS     (GRAVITY_GYRO_OVERRANGE_RATIO * IMU_GYRO_RANGE_DPS)
/* M0 board capture: rest gyro noise peaks at +/-2 deg/s. */
#define GRAVITY_STILL_DPS              (IMU_REST_GYRO_DEPARTURE / (float)INPUT_ROTATION_ONE_DPS)
/* Ours, maintainer choice: clear fast-flip error promptly, outlast a held swing reversal. */
#define GRAVITY_STILL_RECOVER_MS       500U
/* Ours: target 2% settling time for a datasheet-sized gyro offset. */
#define GRAVITY_BIAS_SETTLE_S          20.0F
/* Integral gain in 1/s^2; derivation: docs/input/Gravity.md. */
#define GRAVITY_BIAS_KI                (4.0F / (GRAVITY_TAU_S * GRAVITY_BIAS_SETTLE_S))
/* Acceleration magnitude below which strength is zero, in g. */
#define GRAVITY_FREE_FALL_G            0.3F
/* Maximum prediction and correction interval, in milliseconds. */
#define GRAVITY_MAX_DT_MS              100U

/* Microsecond forms of the limits above. */
#define GRAVITY_RECOVER_US             (GRAVITY_RECOVER_MS * MATH_US_PER_MS)
#define GRAVITY_STILL_RECOVER_US       (GRAVITY_STILL_RECOVER_MS * MATH_US_PER_MS)
#define GRAVITY_MAX_DT_US              (GRAVITY_MAX_DT_MS * MATH_US_PER_MS)

typedef struct {
    /* Unit vector, screen axes: x right, y down, z out. */
    vec3f_t down;
    /* Last acceleration minus down, in g. */
    vec3f_t linear;
    /* Gyro bias in deg/s, learned only from accepted acceleration. */
    vec3f_t gyro_bias;
    /* Last delivered rotation in deg/s, held across missing samples. */
    vec3f_t rotation;
    /* Time-weighted rejection debt, clamped to GRAVITY_RECOVER_US. */
    uint32_t distrust_us;
    /* Continuous still rejection time; motion or acceptance cancels early recovery. */
    uint32_t still_us;
    bool recovering;
    bool primed;
} gravity_t;

/* Reset to down=(0,0,-1), zero linear g and bias deg/s; adopt the first nonzero acceleration exactly. */
void gravity_reset(gravity_t* g);
/* Motion uses INPUT_ACCEL_ONE_G and INPUT_ROTATION_ONE_DPS counts in screen axes.
 * dt_us is clamped to GRAVITY_MAX_DT_US; zero dt skips prediction and blending.
 * Zero acceleration cannot correct down; only the initial prime may snap at zero dt. */
void gravity_update(gravity_t* g, const input_motion_t* m, uint32_t dt_us);
/* Lost-sample duration, bounded by GRAVITY_MAX_DT_US. */
uint32_t gravity_held_us(uint8_t lost);
/* A gap soft-restarts correction; lost time holds the previous rotation before delivered samples. */
void gravity_update_batch(gravity_t* g, const input_motion_batch_t* batch);
/* Unit down vector in screen axes, including during free fall. */
vec3f_t gravity_down(const gravity_t* g);
/* Last acceleration minus down in screen axes, in g; zero after reset. */
vec3f_t gravity_linear(const gravity_t* g);
/* Learned gyro bias in screen axes, in deg/s. */
vec3f_t gravity_gyro_bias(const gravity_t* g);
/* A new rest-window offset absorbs the residual learned by the filter. */
void gravity_clear_gyro_bias(gravity_t* g);
/* sin of tilt from flat, 0..1; zero during free fall. */
float gravity_strength(const gravity_t* g);
/* True when |linear + down| is below GRAVITY_FREE_FALL_G. */
bool gravity_in_free_fall(const gravity_t* g);

/* Full magnitude trust, with acceleration magnitude in g. */
bool gravity_accel_trusted(float magnitude_g);

/* imu_rest: recent-mean stillness and zero-rate windows, without a sensor or clock. */
#pragma once
#include "input/input.h"
#include "math/scalar/units.h"

#define IMU_REST_RECENT_MS       250
#define IMU_REST_ACCEL_DEPARTURE (INPUT_ACCEL_ONE_G * 80 / 1000)
/* Measured rest gyro noise peaks at +/-2 deg/s. */
#define IMU_REST_GYRO_DEPARTURE  (3 * INPUT_ROTATION_ONE_DPS)
#define IMU_REST_SETTLE_MS       1000
#define IMU_REST_WINDOW_MS       2000
#define IMU_REST_MAX_GAP_MS      250

/* Ours: resolve quarter-degree-per-second offsets while rejecting slow tilt, in Q6 deg/s. */
#define IMU_REST_RATE_RESOLUTION (INPUT_ROTATION_ONE_DPS / 4)
/* INPUT_ACCEL_ONE_G * sin(rate_resolution / INPUT_ROTATION_ONE_DPS * (settle_ms + window_ms)
 * / MATH_MS_PER_S * MATH_RADIANS_PER_DEGREE), in Q12 g. */
#define IMU_REST_DRIFT           53.614986F

typedef enum { IMU_REST_MOVING, IMU_REST_SETTLING, IMU_REST_MEASURING, IMU_REST_DONE } imu_rest_stage_t;

/* Recent means reject departures; anchor spans settling and measuring; timers own stage progress. */
typedef struct {
    bool primed;
    int64_t acceleration[3], rotation[3];
    int n;
    float recent_accel[3], recent_gyro[3], anchor[3];
    uint32_t settle_us, window_us;
} imu_rest_t;

void imu_rest_reset(imu_rest_t* rest);
/* DONE restarts the window; dt zero neither advances nor resets. Gate adds the caller's pose constraint. */
imu_rest_stage_t imu_rest_step(imu_rest_t* rest, const input_motion_t* sample, uint32_t dt_us, bool gate);

/* Rounded window mean, valid on DONE; zero for an empty window. */
input_motion_t imu_rest_mean(const imu_rest_t* rest);

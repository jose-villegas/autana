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

typedef struct {
    bool primed;
    float recent_accel[3], recent_gyro[3];
    uint32_t settle_us, window_us;
    int64_t rotation[3];
    uint32_t samples;
} imu_rest_t;

void imu_rest_reset(imu_rest_t* rest);
/* Departure from recent means only; callers own their gravity and window gates. */
bool imu_rest_still(imu_rest_t* rest, const input_motion_t* sample, uint32_t dt_us);
/* True once per settled window near one g; residual is mean corrected Q6 deg/s. */
bool imu_rest_update(imu_rest_t* rest, const input_motion_t* sample, uint32_t dt_us, vec3i_t* residual);

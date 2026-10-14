/* Six-position measurements in panel axes; pose names follow the page quarter. */
#pragma once

#include "input/input.h"

#define IMU_PROBE_MILLI           1000
#define IMU_PROBE_SCALE_ONE       1000000
#define IMU_PROBE_PERCENT         100
#define IMU_PROBE_ACCEL_DEPARTURE (INPUT_ACCEL_ONE_G * 80 / IMU_PROBE_MILLI)
#define IMU_PROBE_GYRO_DEPARTURE  INPUT_ROTATION_ONE_DPS
#define IMU_PROBE_GRAVITY_MIN     (INPUT_ACCEL_ONE_G * 80 / IMU_PROBE_PERCENT)
#define IMU_PROBE_RECENT_MS       250
#define IMU_PROBE_SETTLE_MS       1000
#define IMU_PROBE_WINDOW_MS       2000
#define IMU_PROBE_MAX_GAP_MS      250

typedef enum {
    IMU_PROBE_FACE_UP,
    IMU_PROBE_FACE_DOWN,
    IMU_PROBE_LEFT_DOWN,
    IMU_PROBE_RIGHT_DOWN,
    IMU_PROBE_TOP_DOWN,
    IMU_PROBE_BOTTOM_DOWN,
    IMU_PROBE_POSE_COUNT
} imu_probe_pose_t;

typedef struct {
    int64_t acceleration[3], rotation[3];
    int n;
} imu_probe_average_t;

typedef struct {
    vec3i_t offset_mg, scale_ppm, gyro_mdps;
} imu_probe_fit_t;

typedef struct {
    bool active, primed;
    int pose, quarter;
    uint32_t settle_ms, window_ms;
    float recent_accel[3], recent_gyro[3];
    imu_probe_average_t window, poses[IMU_PROBE_POSE_COUNT];
    imu_probe_fit_t fit;
} imu_probe_round_t;

const char* imu_probe_pose_name(int pose, bool log_name);
vec3i_t imu_probe_pose_direction(int pose, int quarter);
void imu_probe_average_add(imu_probe_average_t* average, input_motion_t sample);
input_motion_t imu_probe_average_read(const imu_probe_average_t* average);
imu_probe_fit_t imu_probe_fit(const imu_probe_average_t poses[IMU_PROBE_POSE_COUNT], int quarter);
void imu_probe_start(imu_probe_round_t* round, int quarter);
/* Returns true once per completed pose. A missing reading must reset priming. */
bool imu_probe_update(imu_probe_round_t* round, input_motion_t sample, uint32_t dt_ms);

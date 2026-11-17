/* imu_probe: six-position measurements and turn timing in panel axes; poses follow the page quarter. */
#pragma once

#include <stddef.h>

#include "input/input.h"
#include "input/motion/imu_calib.h"
#include "input/motion/imu_rest.h"
#include "math/linear/vec3f.h"

#define IMU_PROBE_DECIMAL_DIGITS(value)                                                                                \
    (1 + ((value) >= 10U) + ((value) >= 100U) + ((value) >= 1000U) + ((value) >= 10000U) + ((value) >= 100000U)        \
     + ((value) >= 1000000U) + ((value) >= 10000000U) + ((value) >= 100000000U) + ((value) >= 1000000000U))

#define IMU_PROBE_LABEL_SIZE  16
#define IMU_PROBE_MILLI       1000
#define IMU_PROBE_SCALE_ONE   IMU_CALIB_SCALE_ONE
#define IMU_PROBE_PERCENT     100
#define IMU_PROBE_GRAVITY_MIN (INPUT_ACCEL_ONE_G * 80 / IMU_PROBE_PERCENT)
/* Ours: roughly 41 degrees from bottom-down tolerates a hand hold, but excludes a 45-degree reading tilt. */
#define IMU_PROBE_UPRIGHT_MIN 0.75F

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
    vec3i_t scale_ppm, gyro_mdps;
    imu_calib_t calib;
} imu_probe_fit_t;

/* A fast hand turn exceeds rest noise; one second at 20 Hz exposes settling lag. */
#define IMU_PROBE_TURN_THRESHOLD (45 * INPUT_ROTATION_ONE_DPS)
#define IMU_PROBE_TURN_WINDOW_MS 1000U
#define IMU_PROBE_TURN_SAMPLES   21
#define IMU_PROBE_TURN_PERIOD_MS (IMU_PROBE_TURN_WINDOW_MS / (IMU_PROBE_TURN_SAMPLES - 1))

typedef enum {
    IMU_PROBE_IDLE,
    IMU_PROBE_POSES,
    IMU_PROBE_UPRIGHT,
    IMU_PROBE_TURN,
    IMU_PROBE_TRACE,
    IMU_PROBE_DONE
} imu_probe_step_t;

typedef struct {
    imu_rest_t rest;
    int pose, quarter;
    imu_rest_t poses[IMU_PROBE_POSE_COUNT];
    imu_probe_fit_t fit;
    imu_probe_step_t step;
    uint32_t turn_ms, trace_ms;
    int trace_count;
    vec3f_t trace_down;
} imu_probe_round_t;

const char* imu_probe_pose_name(int pose);
void imu_probe_pose_label(int pose, char* out, size_t size);
vec3i_t imu_probe_pose_direction(int pose, int quarter);
imu_probe_fit_t imu_probe_fit(const imu_rest_t poses[IMU_PROBE_POSE_COUNT], int quarter);
vec3i_t imu_probe_saved_offset(vec3i_t stored, const imu_probe_fit_t* fit);
/* Q12 g counts to mg, rounded. */
vec3i_t imu_probe_mg(vec3i_t q12);
/* Starts pose collection in the supplied page quarter, discarding the previous round. */
void imu_probe_start(imu_probe_round_t* round, int quarter);
/* Returns true once per completed pose. NULL means no reading this frame. */
bool imu_probe_update(imu_probe_round_t* round, const input_motion_t* sample, uint32_t dt_ms);

/* Caller supplies estimated down in panel axes. Returns true only when trace_down and
 * trace_ms hold a new decimated reading; NULL advances trace time without recording. */
bool imu_probe_turn_update(imu_probe_round_t* round, const input_motion_t* sample, vec3f_t down, uint32_t dt_ms);

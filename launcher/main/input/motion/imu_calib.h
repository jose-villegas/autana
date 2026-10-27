/* imu_calib: screen-axis offsets and accelerometer six-position fit (ST AN4508). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"
#include "input/motion/imu.h"
#include "input/motion/imu_rest.h"

#define IMU_CALIB_OFFSET_MAX (INPUT_ACCEL_ONE_G / 4)
#define IMU_CALIB_SCALE_ONE  1000000

typedef struct {
    vec3i_t offset;
} imu_calib_t;

typedef struct {
    imu_calib_t calib;
    vec3i_t scale_ppm;
} imu_calib_fit_t;

/* Each component is its axis's mean reading at +1 g or -1 g. */
imu_calib_fit_t imu_calib_fit(vec3i_t up, vec3i_t down);
/* Correction subtracts only the offset; sensitivity is reported, never applied. */
vec3i_t imu_calib_apply(const imu_calib_t* calib, vec3i_t raw);
bool imu_calib_valid(vec3i_t offset, int32_t max);
/* Uncommitted, missing or excessive offsets invalidate the whole calibration. */
bool imu_calib_from_saved(bool committed, const bool found[3], const int32_t saved[3], int32_t max, imu_calib_t* out);

#define IMU_CALIB_GYRO_MAX ((int32_t)(IMU_GYRO_BIAS_MAX_DPS * INPUT_ROTATION_ONE_DPS))
#define IMU_CALIB_GYRO_RESIDUAL_MAX                                                                                    \
    ((int32_t)(IMU_GYRO_OFFSET_TCO_DPS_PER_C * IMU_GYRO_TEMP_SPAN_C * INPUT_ROTATION_ONE_DPS))
/* Ours: avoid flash wear from small changes and frequent rest windows. */
#define IMU_CALIB_GYRO_SAVE_DELTA       IMU_REST_RATE_RESOLUTION
#define IMU_CALIB_GYRO_SAVE_INTERVAL_US (INT64_C(5) * MATH_MS_PER_MIN * MATH_US_PER_MS)

/* Live correction, committed storage validity, and the earliest next write (0: now). */
typedef struct {
    imu_calib_t live;
    vec3i_t saved;
    int64_t next_save_us;
    bool valid, storage_valid;
} imu_calib_gyro_t;

/* A rejected residual leaves the live offset unchanged. */
typedef enum { IMU_CALIB_GYRO_REJECTED, IMU_CALIB_GYRO_APPLIED, IMU_CALIB_GYRO_SAVE_DUE } imu_calib_gyro_step_t;

imu_calib_gyro_t imu_calib_gyro_from_saved(bool committed, const bool found[3], const int32_t saved[3]);

/* Applies a bounded residual and decides whether storage needs a write. */
imu_calib_gyro_step_t imu_calib_gyro_learn(imu_calib_gyro_t* gyro, vec3i_t residual, int64_t now_us);
/* Subtracts the live offset and recomputes turn speed, including before learning. */
input_motion_t imu_calib_gyro_correct(const imu_calib_gyro_t* gyro, input_motion_t sample);
/* Every attempt starts the wear interval; failure invalidates the stored offset. */
void imu_calib_gyro_saved(imu_calib_gyro_t* gyro, bool ok, int64_t now_us);

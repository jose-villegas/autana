/* imu_calib: screen-axis offsets and accelerometer six-position fit (ST AN4508). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"
#include "input/motion/gravity.h"

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
vec3i_t imu_calib_apply(const imu_calib_t* calib, vec3i_t raw);
bool imu_calib_valid(vec3i_t offset, int32_t max);
/* Uncommitted, missing or excessive offsets invalidate the whole calibration. */
imu_calib_t imu_calib_from_saved(bool committed, const bool found[3], const int32_t saved[3], int32_t max);

#define IMU_CALIB_GYRO_MAX ((int32_t)(GRAVITY_BIAS_MAX_DPS * INPUT_ROTATION_ONE_DPS))
#define IMU_CALIB_GYRO_RESIDUAL_MAX                                                                                    \
    ((int32_t)(GRAVITY_GYRO_OFFSET_TCO_DPS_PER_C * GRAVITY_GYRO_TEMP_SPAN_C * INPUT_ROTATION_ONE_DPS))
/* Ours: avoid flash wear from small changes and frequent rest windows. */
#define IMU_CALIB_GYRO_SAVE_DELTA       (INPUT_ROTATION_ONE_DPS / 4)
#define IMU_CALIB_GYRO_SAVE_INTERVAL_US (INT64_C(5) * MATH_MS_PER_MIN * MATH_US_PER_MS)

typedef struct {
    imu_calib_t live;
    vec3i_t saved;
    int64_t saved_us;
    bool valid, has_saved;
} imu_calib_gyro_t;

bool imu_calib_learn_gyro(imu_calib_gyro_t* gyro, vec3i_t residual);
bool imu_calib_gyro_save_due(const imu_calib_gyro_t* gyro, int64_t now_us);
void imu_calib_gyro_saved(imu_calib_gyro_t* gyro, int64_t now_us);

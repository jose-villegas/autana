/* Accelerometer offset in Q12 g, screen axes. Six-position fit: docs/Citations.md#67.
 * Sensitivity is reported in ppm; correction subtracts only the offset. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/input.h"

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
/* Missing or excessive offsets invalidate the whole calibration. */
imu_calib_t imu_calib_from_saved(const bool found[3], const int32_t saved[3]);

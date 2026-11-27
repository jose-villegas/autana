/* imu_vibration: how far a burst of accelerometer samples moves around its own mean, in mg. */
#pragma once

#include <math.h>
#include <stdbool.h>

#include "input/motion/imu_sample.h"

#define IMU_VIBRATION_MG_PER_G    1000.0F
/* A mean needs one sample and a spread around it a second. */
#define IMU_VIBRATION_MIN_SAMPLES 2

typedef struct {
    float rms_mg;  /* root mean square distance of each sample from the burst mean */
    float peak_mg; /* largest such distance */
    float gx_mg;
    float gy_mg;
    float gz_mg;
    int samples;
} imu_vibration_t;

/* Gravity, sensor offset and slow tilt sit in the mean, so only the shaking is left.
 * False below IMU_VIBRATION_MIN_SAMPLES. */
static inline bool
imu_vibration_measure(const imu_sample_t* samples, int count, imu_vibration_t* out) {
    if (count < IMU_VIBRATION_MIN_SAMPLES) {
        return false;
    }
    float mean[3] = {0};
    for (int i = 0; i < count; i++) {
        mean[0] += (float)samples[i].ax;
        mean[1] += (float)samples[i].ay;
        mean[2] += (float)samples[i].az;
    }
    for (int axis = 0; axis < 3; axis++) {
        mean[axis] /= (float)count;
    }
    float sum_squares = 0;
    float peak_squared = 0;
    for (int i = 0; i < count; i++) {
        const float dx = (float)samples[i].ax - mean[0];
        const float dy = (float)samples[i].ay - mean[1];
        const float dz = (float)samples[i].az - mean[2];
        const float squared = (dx * dx) + (dy * dy) + (dz * dz);
        sum_squares += squared;
        peak_squared = squared > peak_squared ? squared : peak_squared;
    }
    const float mg_per_count = IMU_VIBRATION_MG_PER_G / (float)IMU_COUNTS_PER_G;
    *out = (imu_vibration_t){.rms_mg = sqrtf(sum_squares / (float)count) * mg_per_count,
                             .peak_mg = sqrtf(peak_squared) * mg_per_count,
                             .gx_mg = mean[0] * mg_per_count,
                             .gy_mg = mean[1] * mg_per_count,
                             .gz_mg = mean[2] * mg_per_count,
                             .samples = count};
    return true;
}

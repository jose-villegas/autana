/* imu_rate: sample interval estimated from FIFO drain counts and timestamps. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/imu.h"

/* Ours: two seconds gives one-sample quantisation of about 0.25% at 200 Hz. */
#define IMU_RATE_WINDOW_US (2U * MATH_US_PER_S)
/* Ours: wider than plausible oscillator tolerance, while rejecting broken counts. */
#define IMU_RATE_BOUND_PCT 20U

typedef struct {
    int64_t start_us;
    uint32_t samples;
    uint32_t sample_us;
    bool active;
} imu_rate_t;

/* A zero-initialised state starts nominal; gap includes a failed read. */
uint32_t imu_rate_update(imu_rate_t* rate, int samples, int64_t drain_us, bool gap);

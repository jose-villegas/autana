/*
 * imu: QMI8658 stream FIFO drained by a polling task.
 * imu_read() returns the newest sample read by the task. Register values follow the
 * QST QMI8658A datasheet Rev A (2022).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "input/motion/imu_sample.h"
#include "math/scalar/units.h"

/* Must agree with CTRL3's full-scale code in imu.c. */
#define IMU_GYRO_RANGE_DPS 512.0F

/* QST QMI8658A datasheet Rev A (2022), Table 22 CTRL2/CTRL3 (6DOF);
 * Lewis He, SensorLib GyroODR. We use stream mode, 128 samples,
 * and the datasheet count-before-request drain sequence. */
#define IMU_ODR_BASE_HZ    7174.4
#define IMU_ODR_CODE       5
#define IMU_ODR_DIVIDER    (1U << IMU_ODR_CODE)
#define IMU_ODR_HZ         (IMU_ODR_BASE_HZ / IMU_ODR_DIVIDER)
#define IMU_SAMPLE_US      ((uint32_t)((MATH_US_PER_S / IMU_ODR_HZ) + 0.5))
#define IMU_FIFO_SAMPLES   128

/* QMI8658A datasheet section 8.7: read mode discards arrivals, so loss is
 * locked time / IMU_SAMPLE_US. Carry fractional time across drains; gaps break continuity. */
static inline int
imu_fifo_lost(uint64_t locked_us, uint32_t* remainder_us, bool gap) {
    if (gap) {
        *remainder_us = 0;
        return 0;
    }
    const uint64_t total_us = locked_us + *remainder_us;
    *remainder_us = (uint32_t)(total_us % IMU_SAMPLE_US);
    return (int)(total_us / IMU_SAMPLE_US);
}

/* One consumer copies queued samples oldest first; loss precedes each sample.
 * Gaps (bus error, overflow, loss saturation) are reported through *gap and precede samples[0].
 * Returns -1 only without a sensor. */
int imu_take_fifo(imu_sample_t out[IMU_FIFO_SAMPLES], uint8_t lost_before[IMU_FIFO_SAMPLES], bool* gap);

/* Configures and starts the sensor. Safe to call more than once.
 * Returns false if the chip does not answer or identifies as something else. */
bool imu_init(void);

/* Starts the polling task on the core-1 job worker's core, one priority below it.
 * Safe when the sensor is absent. */
void imu_start(void);

bool imu_ready(void);

/* Copies the newest sample read by the task. False when no sensor or no sample is available. */
bool imu_read(imu_sample_t* out);

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Makes imu_read() report `sample` until imu_inject_release() hands back to
 * the sensor. */
void imu_inject(const imu_sample_t* sample);
void imu_inject_release(void);
#endif

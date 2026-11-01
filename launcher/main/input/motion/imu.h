/*
 * imu: QMI8658 stream FIFO drained by a polling task.
 * imu_read() returns the newest sample read by the task. Register values follow the
 * QST QMI8658A datasheet Rev A (2022).
 * The task works only for recent readers (imu_demand.h): FIFO takes keep the drains,
 * imu_read() alone keeps register reads, and with neither it parks until the next call.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "input/motion/imu_sample.h"
#include "math/scalar/units.h"

/* Must agree with CTRL3's full-scale code in imu.c. */
#define IMU_GYRO_RANGE_DPS            512.0F
/* QMI8658C datasheet, Table 8: board-level initial gyro offset tolerance. */
#define IMU_GYRO_OFFSET_TOL_DPS       10.0F
/* QMI8658C datasheet, Table 8: gyro offset temperature coefficient. */
#define IMU_GYRO_OFFSET_TCO_DPS_PER_C 0.05F
/* QMI8658C datasheet, Table 8: from the 25 C reference to the 85 C operating limit. */
#define IMU_GYRO_TEMP_SPAN_C          60.0F
/* QMI8658C datasheet, Table 8: initial offset plus temperature drift, in deg/s. */
#define IMU_GYRO_BIAS_MAX_DPS         (IMU_GYRO_OFFSET_TOL_DPS + (IMU_GYRO_OFFSET_TCO_DPS_PER_C * IMU_GYRO_TEMP_SPAN_C))

/* QST QMI8658A datasheet Rev A (2022), Table 22 CTRL2/CTRL3 (6DOF);
 * Lewis He, SensorLib GyroODR. We use stream mode, 128 samples,
 * and the datasheet count-before-request drain sequence. */
#define IMU_ODR_BASE_HZ               7174.4
#define IMU_ODR_CODE                  5
#define IMU_ODR_DIVIDER               (1U << IMU_ODR_CODE)
#define IMU_ODR_HZ                    (IMU_ODR_BASE_HZ / IMU_ODR_DIVIDER)
#define IMU_SAMPLE_US                 ((uint32_t)((MATH_US_PER_S / IMU_ODR_HZ) + 0.5))
#define IMU_FIFO_SAMPLES              128

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

/* One consumer copies queued samples oldest first, at IMU_ODR_CODE; loss precedes each sample.
 * Gaps (bus error, overflow, loss saturation) are reported through *gap and precede samples[0].
 * Returns -1 only without a sensor. */
int imu_take_fifo(imu_sample_t out[IMU_FIFO_SAMPLES], uint8_t lost_before[IMU_FIFO_SAMPLES], bool* gap);

/* Soft-resets, configures and starts the sensor. Safe to call more than once.
 * Returns false if the chip does not answer or identifies as something else. */
bool imu_init(void);

/* Starts the polling task on the core-1 job worker's core, one priority below it.
 * It parks until the first read. Safe when the sensor is absent. */
void imu_start(void);

bool imu_ready(void);

/* Copies the newest sample and its sequence (when non-NULL) under one lock.
 * False when no sensor or no sample is available. After a park the sample is the
 * last one read before it, until the woken task advances the sequence. */
bool imu_read(imu_sample_t* out, uint32_t* sequence);

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
/* Vibration measurements only: ODR code 3 is IMU_ODR_BASE_HZ / 8, whose
 * default accelerometer bandwidth (half the ODR, Table 11) reaches the speaker's low tones. */
#define IMU_BUZZ_ODR_CODE 3U
/* Asks the IMU task to run the sensor at another ODR code until asked for IMU_ODR_CODE again,
 * and waits until it has. Every switch resets the FIFO and queues a gap, so imu_take_fifo()
 * never mixes rates; between the switches samples are not IMU_SAMPLE_US apart.
 * False without a sensor or task, on a bus error, or when the task does not answer in time.
 * Blocks up to two of the slowest IMU task periods: console task only, never the frame thread. */
bool imu_set_odr(unsigned odr_code);
/* imu_take_fifo() for samples queued at `odr_code`. A take at the wrong rate, by either
 * reader, drops the queue behind a gap, so a reader at the other rate sees it lost them. */
int imu_take_fifo_at(unsigned odr_code, imu_sample_t out[IMU_FIFO_SAMPLES], uint8_t lost_before[IMU_FIFO_SAMPLES],
                     bool* gap);
#endif

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Makes imu_read() report `sample` until imu_inject_release() hands back to
 * the sensor. */
void imu_inject(const imu_sample_t* sample);
void imu_inject_release(void);
#endif

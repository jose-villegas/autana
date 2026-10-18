/*
 * imu: QMI8658 stream FIFO drained by imu_read_fifo().
 * imu_read() returns the latest single sample. Register values follow the
 * QMI8658A datasheet, docs/Citations.md#78.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "input/imu_sample.h"
#include "math/scalar/units.h"

/* Must agree with CTRL3's full-scale code in imu.c. */
#define IMU_GYRO_RANGE_DPS 512.0F

/* QMI8658A Rev A, Table 22 CTRL2/CTRL3 (6DOF), docs/Citations.md#78;
 * SensorLib GyroODR, docs/Citations.md#79. We use stream mode, 128 samples,
 * and the datasheet count-before-request drain sequence. */
#define IMU_ODR_BASE_HZ    7174.4
#define IMU_ODR_CODE       5
#define IMU_ODR_DIVIDER    (1U << IMU_ODR_CODE)
#define IMU_ODR_HZ         (IMU_ODR_BASE_HZ / IMU_ODR_DIVIDER)
#define IMU_SAMPLE_US      ((uint32_t)((MATH_US_PER_S / IMU_ODR_HZ) + 0.5))
#define IMU_FIFO_SAMPLES   128

/* Oldest first; produced and lost refer to pre-request snapshots. Overflow marks a gap.
 * snapshot_us is the host time of the counter snapshot. Returns -1 on a bus error. */
int imu_read_fifo(imu_sample_t out[IMU_FIFO_SAMPLES], int* produced, int* lost, int64_t* snapshot_us, bool* overflowed);

/* Configures and starts the sensor. Safe to call more than once.
 * Returns false if the chip does not answer or identifies as something else. */
bool imu_init(void);

bool imu_ready(void);

/* Reads the latest six axes. Returns false on a bus error. */
bool imu_read(imu_sample_t* out);

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Makes imu_read() report `sample` until imu_inject_release() hands back to
 * the sensor. */
void imu_inject(const imu_sample_t* sample);
void imu_inject_release(void);
#endif

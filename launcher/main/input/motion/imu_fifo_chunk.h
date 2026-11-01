/* imu_fifo_chunk: whole-sample FIFO transactions bounded by a bus-hold target. */
#pragma once

#include "input/motion/imu.h"

/* QMI8658A Rev A, Table 43: maximum I2C clock. */
#define QMI8658_I2C_HZ         400000
#define IMU_BUS_HOLD_TARGET_US 1000
/* Each byte takes 8 data bits plus ACK. */
#define IMU_I2C_BITS_PER_BYTE  9
#define IMU_FIFO_SAMPLE_BUS_US                                                                                         \
    ((sizeof(imu_sample_t) * IMU_I2C_BITS_PER_BYTE * MATH_US_PER_S + QMI8658_I2C_HZ - 1) / QMI8658_I2C_HZ)
#define IMU_FIFO_CHUNK_SAMPLES (IMU_BUS_HOLD_TARGET_US / IMU_FIFO_SAMPLE_BUS_US)

_Static_assert(IMU_FIFO_CHUNK_SAMPLES >= 1, "FIFO chunk must hold a sample");
_Static_assert(IMU_FIFO_CHUNK_SAMPLES <= IMU_FIFO_SAMPLES, "FIFO chunk must fit retained history");

static inline int
imu_fifo_chunk_samples(int remaining) {
    return remaining > (int)IMU_FIFO_CHUNK_SAMPLES ? (int)IMU_FIFO_CHUNK_SAMPLES : remaining;
}

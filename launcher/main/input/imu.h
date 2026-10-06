/*
 * imu: the QMI8658 six-axis accelerometer and gyroscope.
 *
 * Sits on the shared I2C bus at 0x6b, the same bus POST probes. There is no
 * driver for it in the BSP, so this is written against the QST datasheet.
 *
 * Which sensor answers which question is worth being clear about, because it
 * is easy to reach for the wrong one:
 *
 *   - the ACCELEROMETER senses gravity, so it says which way is down. That is
 *     what tilting the board changes, and what steering by tilt wants.
 *   - the GYROSCOPE senses rotation RATE, which is zero however the board is
 *     tilted, as long as it is being held still. It is what tells you the
 *     board is being shaken or spun.
 *
 * Both are read in one transfer (the data registers are contiguous), so using
 * both costs nothing over using either.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "input/imu_sample.h"
#include "util/build/build_variant.h"

/* Configures and starts the sensor. Safe to call more than once.
 * Returns false if the chip does not answer or identifies as something else. */
bool imu_init(void);

bool imu_ready(void);

/* Reads all six axes. Returns false on a bus error, leaving `out` untouched. */
bool imu_read(imu_sample_t* out);

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Makes imu_read() report `sample` until imu_inject_release() hands back to
 * the sensor. */
void imu_inject(const imu_sample_t* sample);
void imu_inject_release(void);
#endif

/*
 * imu - the QMI8658 six-axis accelerometer and gyroscope.
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
 * Both are read in one transfer - the data registers are contiguous - so using
 * both costs nothing over using either.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build_variant.h"

/* Raw sensor counts, in the chip's own axes.
 *
 * Left raw on purpose: a caller steering by tilt needs only the direction of
 * the acceleration vector and the magnitude of the rotation, and both survive
 * scaling. Converting to g and deg/s would cost floating point in the frame
 * loop and buy nothing. The scale factors are here for anyone who does need
 * real units. */
typedef struct {
    int16_t ax, ay, az; /* accelerometer, 4096 counts per g   (+/- 8 g)   */
    int16_t gx, gy, gz; /* gyroscope,       64 counts per dps (+/- 512)   */
} imu_sample_t;

#define IMU_COUNTS_PER_G   4096
#define IMU_COUNTS_PER_DPS 64

/* Configures and starts the sensor. Safe to call more than once.
 * Returns false if the chip does not answer or identifies as something else. */
bool imu_init(void);

bool imu_ready(void);

/* Reads all six axes. Returns false on a bus error, leaving `out` untouched. */
bool imu_read(imu_sample_t* out);

#if CONFIG_LAUNCHER_DEVELOPMENT
/* Makes imu_read() return `sample` until imu_inject_release() restores the
 * controller. */
void imu_inject(const imu_sample_t* sample);
void imu_inject_release(void);
#endif

/* Sensor axes to screen axes: how the QMI8658 is soldered relative to the
 * panel is a board layout fact no datasheet carries, so both facts here come
 * from tilting the board - held upright the sensor reads about +1 g on its
 * X axis and roughly zero on Y, so the chip's X runs down the screen and its
 * Y runs across it pointing left, hence the negation. */
static inline int
imu_gravity_screen_x(const imu_sample_t* s) {
    return -s->ay;
}

static inline int
imu_gravity_screen_y(const imu_sample_t* s) {
    return s->ax;
}

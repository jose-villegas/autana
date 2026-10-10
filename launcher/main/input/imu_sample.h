/*
 * imu_sample: one accelerometer and gyroscope reading, and the pure
 * arithmetic that turns it into screen-space gravity. No bus and no driver,
 * so it links and runs on a host; imu.h is the sensor that fills it.
 */
#pragma once

#include <stdint.h>

#include "math/linear/vec2i.h"

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

/* Sensor axes to screen axes: how the QMI8658 is soldered relative to the
 * panel is a board layout fact no datasheet carries, so both facts here come
 * from tilting the board: held upright the sensor reads about +1 g on its
 * X axis and roughly zero on Y, so the chip's X runs down the screen and its
 * Y runs across it pointing left, hence the negation. */
static inline vec2i_t
imu_gravity_screen(const imu_sample_t* s) {
    return (vec2i_t){-s->ay, s->ax};
}

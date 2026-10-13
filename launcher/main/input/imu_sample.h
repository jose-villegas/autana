/*
 * imu_sample: one accelerometer and gyroscope reading, and the pure
 * arithmetic that turns it into screen-space gravity and a turn rate. No
 * bus and no driver, so it links and runs on a host; imu.h is the sensor
 * that fills it.
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

/* How fast the board is TURNING, 0-255 from the gyroscope's total rotation
 * rate, saturating rather than wrapping. Named for what it measures because
 * the obvious misreading is expensive: this is NOT how hard the board is
 * being shaken; a smooth rotation pins it at maximum while nothing is
 * shaken. Shaking means accelerating the device, the accelerometer's
 * business; see tilt_shake(). Deliberately not a filter or gesture
 * detector; the caller decides what counts as "fast". */
static inline int
imu_rotation_level(const imu_sample_t* s) {
    /* Sum of absolute rates rather than a true vector magnitude: it needs no
     * square root, and for "how fast is this turning" the difference is not
     * perceptible. */
    const int gx = s->gx < 0 ? -s->gx : s->gx;
    const int gy = s->gy < 0 ? -s->gy : s->gy;
    const int gz = s->gz < 0 ? -s->gz : s->gz;

    /* About 300 dps summed across the axes reads as fully turning: brisk, but
     * reachable with a flick of the wrist. */
    const int total = (gx + gy + gz) / IMU_COUNTS_PER_DPS;
    const int level = total * 255 / 300;

    return level > 255 ? 255 : level;
}

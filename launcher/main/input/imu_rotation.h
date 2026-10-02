/*
 * imu_rotation: pure interpretation of the gyroscope's own rotation rate.
 *
 * It touches no I2C and uses only integer arithmetic, so host tests can
 * exercise it. Header-only and ESP-IDF-free.
 */
#pragma once

#include "input/imu_sample.h"

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

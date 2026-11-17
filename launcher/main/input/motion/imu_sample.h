/*
 * imu_sample: one accelerometer and gyroscope reading, and the pure
 * arithmetic that turns it into screen-space gravity and a turn speed. No
 * bus and no driver, so it links and runs on a host; imu.h is the sensor
 * that fills it.
 */
#pragma once

#include <stdint.h>

#include "math/linear/vec3i.h"

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

_Static_assert(sizeof(imu_sample_t) == 12, "FIFO order is six little-endian int16 axes");

#define IMU_COUNTS_PER_G   4096
#define IMU_COUNTS_PER_DPS 64
#define IMU_SAMPLE_UPRIGHT {.ax = IMU_COUNTS_PER_G}

/* Sensor axes to the screen labels x right, y down, z out. The chip sits with X
 * up the screen, Y right and Z out of the back (board/board_geometry.h). It reads
 * specific force, which points up at rest, and callers treat the vector as down,
 * so each label is a chip axis negated: held upright the chip reads +1 g on X,
 * which lands on +y, down. The labels are left-handed; the numbers are true down. */
static inline vec3i_t
imu_axes_screen(int x, int y, int z) {
    return (vec3i_t){-y, x, z};
}

static inline vec2i_t
imu_gravity_screen(const imu_sample_t* s) {
    return vec3i_xy(imu_axes_screen(s->ax, s->ay, s->az));
}

/* Smooth rotation can read high without linear acceleration.
 * Use input_shake() for deliberate shaking, not summed gyro rate. */
static inline int
imu_turn_speed(vec3i_t rotation) {
    /* Sum of absolute rates rather than a true vector magnitude: it needs no
     * square root, and for "how fast is this turning" the difference is not
     * perceptible. */
    const int gx = rotation.x < 0 ? -rotation.x : rotation.x;
    const int gy = rotation.y < 0 ? -rotation.y : rotation.y;
    const int gz = rotation.z < 0 ? -rotation.z : rotation.z;

    return (gx + gy + gz) / IMU_COUNTS_PER_DPS;
}

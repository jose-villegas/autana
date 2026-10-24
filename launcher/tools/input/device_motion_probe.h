/* device_motion_probe: a scripted device pose, read as the QMI8658 would read it, for the input figure probes.
 * A pose's columns are device x, y, z (x right, y up the screen, z out of the glass) in world axes; world z is up. */
#pragma once

#include <math.h>
#include <stdio.h>

#include "board/board_geometry.h"
#include "input/motion/imu_sample.h"

#define PROBE_PI         3.14159265F
#define PROBE_DEG_TO_RAD (PROBE_PI / 180.0F)

typedef struct {
    float m[3][3];
} probe_pose_t;

typedef probe_pose_t (*probe_pose_at_t)(float t);

static const float probe_chip_axes[3][3] = {BOARD_IMU_AXIS_X, BOARD_IMU_AXIS_Y, BOARD_IMU_AXIS_Z};

/* Where imu_axes_screen() sends each chip axis: per screen label x, y, z, a device vector. */
static inline void
probe_label_axes(float out[3][3]) {
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < 3; i++) {
            out[s][i] = 0;
        }
        for (int c = 0; c < 3; c++) {
            const vec3i_t unit = imu_axes_screen(c == 0, c == 1, c == 2);
            const int along = s == 0 ? unit.x : s == 1 ? unit.y : unit.z;
            for (int i = 0; i < 3; i++) {
                out[s][i] += (float)along * probe_chip_axes[c][i];
            }
        }
    }
}

static inline void
probe_rotate(float out[3][3], const float a[3][3], const float b[3][3]) {
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            out[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        }
    }
}

/* World vector w in device axes. */
static inline void
probe_to_device(const probe_pose_t* p, const float w[3], float d[3]) {
    for (int i = 0; i < 3; i++) {
        d[i] = p->m[0][i] * w[0] + p->m[1][i] * w[1] + p->m[2][i] * w[2];
    }
}

static inline float
probe_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* Body rate in device axes, rad/s, from R^T R': the skew part over dt. */
static inline void
probe_body_rate(probe_pose_at_t pose, float t, float dt, float rate[3]) {
    const probe_pose_t now = pose(t), next = pose(t + dt);
    float rel[3][3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            rel[r][c] = now.m[0][r] * next.m[0][c] + now.m[1][r] * next.m[1][c] + now.m[2][r] * next.m[2][c];
        }
    }
    rate[0] = (rel[2][1] - rel[1][2]) / (2 * dt);
    rate[1] = (rel[0][2] - rel[2][0]) / (2 * dt);
    rate[2] = (rel[1][0] - rel[0][1]) / (2 * dt);
}

/* What the chip reads at rest in that pose: specific force (up, 1 g) and the body rate plus a per-chip-axis
 * offset in deg/s, in chip counts. */
static inline imu_sample_t
probe_chip_sample(probe_pose_at_t pose, float t, float dt, const float offset_dps[3]) {
    const probe_pose_t now = pose(t);
    float up[3], rate[3];
    probe_to_device(&now, (const float[3]){0, 0, 1}, up);
    probe_body_rate(pose, t, dt, rate);
    int16_t accel[3], gyro[3];
    for (int i = 0; i < 3; i++) {
        accel[i] = (int16_t)lroundf(probe_dot(up, probe_chip_axes[i]) * IMU_COUNTS_PER_G);
        gyro[i] = (int16_t)lroundf((probe_dot(rate, probe_chip_axes[i]) / PROBE_DEG_TO_RAD + offset_dps[i])
                                   * IMU_COUNTS_PER_DPS);
    }
    return (imu_sample_t){accel[0], accel[1], accel[2], gyro[0], gyro[1], gyro[2]};
}

static inline void
probe_print_pose(const probe_pose_t* p) {
    printf("\"pose\":[");
    for (int r = 0; r < 3; r++) {
        printf("%s[%g,%g,%g]", r ? "," : "", (double)p->m[r][0], (double)p->m[r][1], (double)p->m[r][2]);
    }
    printf("]");
}

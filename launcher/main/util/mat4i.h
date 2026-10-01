/*
 * mat4i: integer 4x4 transform maths, 512 to a unit and 512 to a turn, after
 * small3dlib (drummyfish, CC0). The line camera, the animation transforms and
 * the boot scene share it.
 *
 * Integer, so the same inputs give the same pixel on the board, on x86 and
 * in a pinned host render. The sine is trig.h's, rescaled to this unit, and
 * every product accumulates in 64 bits and rounds once through fixed.h, so
 * coordinates are bounded by int32 only after the product, not within it.
 * Header-only, static inline and ESP-IDF-free.
 */
#pragma once

#include <stdint.h>

#include "util/fixed.h"
#include "util/trig.h"

#define M4_SHIFT 9
#define M4_ONE   (1 << M4_SHIFT)

typedef int32_t m4_unit_t;

typedef struct {
    m4_unit_t x, y, z, w;
} m4_vec4_t;

/* Indexed [column][row], acting on column vectors. */
typedef m4_unit_t m4_mat_t[4][4];

/* Rotation is Euler angles in turns, applied Z, then X, then Y. */
typedef struct {
    m4_vec4_t translation;
    m4_vec4_t rotation;
    m4_vec4_t scale;
} m4_transform_t;

static inline m4_unit_t
m4_non_zero(m4_unit_t value) {
    return value + (value == 0);
}

/* Any angle in turns, negative included: the phase wraps by truncation. */
static inline m4_unit_t
m4_sin(m4_unit_t angle) {
    const uint16_t phase = (uint16_t)((uint32_t)angle * (65536u / M4_ONE));
    return (m4_unit_t)fx_round_shift(trig_sin(phase), 15 - M4_SHIFT);
}

static inline m4_unit_t
m4_cos(m4_unit_t angle) {
    return m4_sin(angle + (M4_ONE / 4));
}

static inline void
m4_vec4_init(m4_vec4_t* v) {
    *v = (m4_vec4_t){0, 0, 0, M4_ONE};
}

static inline void
m4_transform_init(m4_transform_t* t) {
    m4_vec4_init(&t->translation);
    m4_vec4_init(&t->rotation);
    t->scale = (m4_vec4_t){M4_ONE, M4_ONE, M4_ONE, 0};
}

static inline void
m4_mat_init(m4_mat_t m) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            m[c][r] = c == r ? M4_ONE : 0;
        }
    }
}

static inline void
m4_mat_copy(m4_mat_t src, m4_mat_t dst) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            dst[c][r] = src[c][r];
        }
    }
}

/* m1 = m1 * m2. */
static inline void
m4_mat_mul(m4_mat_t m1, m4_mat_t m2) {
    m4_mat_t a;
    m4_mat_copy(m1, a);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int64_t sum = 0;
            for (int i = 0; i < 4; i++) {
                sum += (int64_t)a[i][r] * m2[c][i];
            }
            m1[c][r] = (m4_unit_t)fx_round_shift(sum, M4_SHIFT);
        }
    }
}

/* One row of a point times a matrix, translation included, rounded once. */
static inline m4_unit_t
m4_row_dot(m4_vec4_t in, m4_mat_t m, int row) {
    const int64_t sum = ((int64_t)in.x * m[row][0]) + ((int64_t)in.y * m[row][1]) + ((int64_t)in.z * m[row][2]);
    return (m4_unit_t)fx_round_shift(sum, M4_SHIFT) + m[row][3];
}

/* Point times matrix, translation included; w comes back M4_ONE. */
static inline void
m4_vec3_transform(m4_vec4_t* v, m4_mat_t m) {
    const m4_vec4_t in = *v;

    v->x = m4_row_dot(in, m, 0);
    v->y = m4_row_dot(in, m, 1);
    v->z = m4_row_dot(in, m, 2);
    v->w = M4_ONE;
}

/* Divides x and y by z, scaled by the focal length; 0 is orthographic. Does
 * not check z. */
static inline void
m4_perspective_divide(m4_vec4_t* v, m4_unit_t focal) {
    if (focal == 0) {
        return;
    }
    v->x = (v->x * focal) / v->z;
    v->y = (v->y * focal) / v->z;
}

static inline void
m4_translation_matrix(m4_unit_t x, m4_unit_t y, m4_unit_t z, m4_mat_t m) {
    m4_mat_init(m);
    m[0][3] = x;
    m[1][3] = y;
    m[2][3] = z;
}

static inline void
m4_scale_matrix(m4_unit_t x, m4_unit_t y, m4_unit_t z, m4_mat_t m) {
    m4_mat_init(m);
    m[0][0] = x;
    m[1][1] = y;
    m[2][2] = z;
}

/* A sum of unit-cubed terms, back to one unit. */
static inline m4_unit_t
m4_q18(int64_t sum) {
    return (m4_unit_t)fx_round_shift(sum, 2 * M4_SHIFT);
}

/* Z, then X, then Y; each angle in turns, negated before its sine. */
static inline void
m4_rotation_matrix(m4_unit_t by_x, m4_unit_t by_y, m4_unit_t by_z, m4_mat_t m) {
    const m4_unit_t sx = m4_sin(-by_x);
    const m4_unit_t sy = m4_sin(-by_y);
    const m4_unit_t sz = m4_sin(-by_z);
    const m4_unit_t cx = m4_cos(-by_x);
    const m4_unit_t cy = m4_cos(-by_y);
    const m4_unit_t cz = m4_cos(-by_z);
    const int64_t s = M4_ONE;

    m[0][0] = m4_q18((cy * cz * s) + (sy * sx * sz));
    m[1][0] = fx_mul_round(cx, sz, M4_SHIFT);
    m[2][0] = m4_q18((cy * sx * sz) - (cz * sy * s));
    m[3][0] = 0;

    m[0][1] = m4_q18((cz * sy * sx) - (cy * sz * s));
    m[1][1] = fx_mul_round(cx, cz, M4_SHIFT);
    m[2][1] = m4_q18((cy * cz * sx) + (sy * sz * s));
    m[3][1] = 0;

    m[0][2] = fx_mul_round(cx, sy, M4_SHIFT);
    m[1][2] = -sx;
    m[2][2] = fx_mul_round(cy, cx, M4_SHIFT);
    m[3][2] = 0;

    m[0][3] = 0;
    m[1][3] = 0;
    m[2][3] = 0;
    m[3][3] = M4_ONE;
}

/* Model to world: scale, then rotate, then translate. */
static inline void
m4_world_matrix(m4_transform_t world, m4_mat_t m) {
    m4_mat_t t;

    m4_scale_matrix(world.scale.x, world.scale.y, world.scale.z, m);
    m4_rotation_matrix(world.rotation.x, world.rotation.y, world.rotation.z, t);
    m4_mat_mul(m, t);
    m4_translation_matrix(world.translation.x, world.translation.y, world.translation.z, t);
    m4_mat_mul(m, t);
}

/* World to camera: the inverse of the camera's own pose. */
static inline void
m4_camera_matrix(m4_transform_t camera, m4_mat_t m) {
    m4_mat_t r, s;

    m4_translation_matrix(-camera.translation.x, -camera.translation.y, -camera.translation.z, m);
    m4_rotation_matrix(camera.rotation.x, camera.rotation.y, camera.rotation.z, r);
    for (int y = 0; y < 3; y++) {
        for (int x = y + 1; x < 4; x++) {
            const m4_unit_t tmp = r[x][y];
            r[x][y] = r[y][x];
            r[y][x] = tmp;
        }
    }
    m4_scale_matrix(camera.scale.x, camera.scale.y, camera.scale.z, s);
    m4_mat_mul(m, r);
    m4_mat_mul(m, s);
}

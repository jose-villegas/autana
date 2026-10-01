/*
 * matrix4i: integer 4x4 transform maths over vec4i.h's unit, after
 * small3dlib (drummyfish, CC0). The line camera, the animation transforms and
 * the boot scene share it.
 *
 * Integer, so the same inputs give the same pixel on the board, on x86 and
 * in a pinned host render. The sine is trig.h's, rescaled to this unit, and
 * each sum of products rounds once through fixed.h. Sums are plain int32, so
 * a caller keeps its coordinates small enough not to overflow (boot_anim.h
 * states its bound). Header-only, static inline and ESP-IDF-free.
 */
#pragma once

#include <stdint.h>

#include "util/fixed.h"
#include "util/math/vec4i.h"
#include "util/trig.h"

/* Indexed [column][row], acting on column vectors. */
typedef vec4i_unit_t matrix4i_t[4][4];

/* Rotation is Euler angles in turns, applied Z, then X, then Y. */
typedef struct {
    vec4i_t translation;
    vec4i_t rotation;
    vec4i_t scale;
} matrix4i_transform_t;

/* Any angle in turns, negative included: the phase wraps by truncation. */
static inline vec4i_unit_t
matrix4i_sin(vec4i_unit_t angle) {
    const uint16_t phase = (uint16_t)((uint32_t)angle * (65536u / VEC4I_ONE));
    return (vec4i_unit_t)fx_round_shift(trig_sin(phase), 15 - VEC4I_SHIFT);
}

static inline vec4i_unit_t
matrix4i_cos(vec4i_unit_t angle) {
    return matrix4i_sin(angle + (VEC4I_ONE / 4));
}

static inline void
matrix4i_transform_init(matrix4i_transform_t* t) {
    vec4i_init(&t->translation);
    vec4i_init(&t->rotation);
    t->scale = (vec4i_t){VEC4I_ONE, VEC4I_ONE, VEC4I_ONE, 0};
}

static inline void
matrix4i_init(matrix4i_t m) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            m[c][r] = c == r ? VEC4I_ONE : 0;
        }
    }
}

static inline void
matrix4i_copy(matrix4i_t src, matrix4i_t dst) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            dst[c][r] = src[c][r];
        }
    }
}

/* m1 = m1 * m2. */
static inline void
matrix4i_mul(matrix4i_t m1, matrix4i_t m2) {
    matrix4i_t a;
    matrix4i_copy(m1, a);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int32_t sum = 0;
            for (int i = 0; i < 4; i++) {
                sum += a[i][r] * m2[c][i];
            }
            m1[c][r] = fx_round_shift32(sum, VEC4I_SHIFT);
        }
    }
}

/* One row of a point times a matrix, translation included, rounded once. */
static inline vec4i_unit_t
matrix4i_row_dot(vec4i_t in, matrix4i_t m, int row) {
    const int32_t sum = (in.x * m[row][0]) + (in.y * m[row][1]) + (in.z * m[row][2]);
    return fx_round_shift32(sum, VEC4I_SHIFT) + m[row][3];
}

/* Point times matrix, translation included; w comes back VEC4I_ONE. */
static inline void
matrix4i_transform_point(vec4i_t* v, matrix4i_t m) {
    const vec4i_t in = *v;

    v->x = matrix4i_row_dot(in, m, 0);
    v->y = matrix4i_row_dot(in, m, 1);
    v->z = matrix4i_row_dot(in, m, 2);
    v->w = VEC4I_ONE;
}

/* Divides x and y by z, scaled by the focal length; 0 is orthographic. Does
 * not check z. */
static inline void
matrix4i_perspective_divide(vec4i_t* v, vec4i_unit_t focal) {
    if (focal == 0) {
        return;
    }
    v->x = (v->x * focal) / v->z;
    v->y = (v->y * focal) / v->z;
}

static inline void
matrix4i_translation(vec4i_unit_t x, vec4i_unit_t y, vec4i_unit_t z, matrix4i_t m) {
    matrix4i_init(m);
    m[0][3] = x;
    m[1][3] = y;
    m[2][3] = z;
}

static inline void
matrix4i_scale(vec4i_unit_t x, vec4i_unit_t y, vec4i_unit_t z, matrix4i_t m) {
    matrix4i_init(m);
    m[0][0] = x;
    m[1][1] = y;
    m[2][2] = z;
}

/* A sum of unit-cubed terms, back to one unit. */
static inline vec4i_unit_t
matrix4i_q18(int32_t sum) {
    return fx_round_shift32(sum, 2 * VEC4I_SHIFT);
}

/* Z, then X, then Y; each angle in turns, negated before its sine. */
static inline void
matrix4i_rotation(vec4i_unit_t by_x, vec4i_unit_t by_y, vec4i_unit_t by_z, matrix4i_t m) {
    const vec4i_unit_t sx = matrix4i_sin(-by_x);
    const vec4i_unit_t sy = matrix4i_sin(-by_y);
    const vec4i_unit_t sz = matrix4i_sin(-by_z);
    const vec4i_unit_t cx = matrix4i_cos(-by_x);
    const vec4i_unit_t cy = matrix4i_cos(-by_y);
    const vec4i_unit_t cz = matrix4i_cos(-by_z);
    const int32_t s = VEC4I_ONE;

    m[0][0] = matrix4i_q18((cy * cz * s) + (sy * sx * sz));
    m[1][0] = fx_mul_round(cx, sz, VEC4I_SHIFT);
    m[2][0] = matrix4i_q18((cy * sx * sz) - (cz * sy * s));
    m[3][0] = 0;

    m[0][1] = matrix4i_q18((cz * sy * sx) - (cy * sz * s));
    m[1][1] = fx_mul_round(cx, cz, VEC4I_SHIFT);
    m[2][1] = matrix4i_q18((cy * cz * sx) + (sy * sz * s));
    m[3][1] = 0;

    m[0][2] = fx_mul_round(cx, sy, VEC4I_SHIFT);
    m[1][2] = -sx;
    m[2][2] = fx_mul_round(cy, cx, VEC4I_SHIFT);
    m[3][2] = 0;

    m[0][3] = 0;
    m[1][3] = 0;
    m[2][3] = 0;
    m[3][3] = VEC4I_ONE;
}

/* Model to world: scale, then rotate, then translate. */
static inline void
matrix4i_world(matrix4i_transform_t world, matrix4i_t m) {
    matrix4i_t t;

    matrix4i_scale(world.scale.x, world.scale.y, world.scale.z, m);
    matrix4i_rotation(world.rotation.x, world.rotation.y, world.rotation.z, t);
    matrix4i_mul(m, t);
    matrix4i_translation(world.translation.x, world.translation.y, world.translation.z, t);
    matrix4i_mul(m, t);
}

/* World to camera: the inverse of the camera's own pose. */
static inline void
matrix4i_camera(matrix4i_transform_t camera, matrix4i_t m) {
    matrix4i_t r, s;

    matrix4i_translation(-camera.translation.x, -camera.translation.y, -camera.translation.z, m);
    matrix4i_rotation(camera.rotation.x, camera.rotation.y, camera.rotation.z, r);
    for (int y = 0; y < 3; y++) {
        for (int x = y + 1; x < 4; x++) {
            const vec4i_unit_t tmp = r[x][y];
            r[x][y] = r[y][x];
            r[y][x] = tmp;
        }
    }
    matrix4i_scale(camera.scale.x, camera.scale.y, camera.scale.z, s);
    matrix4i_mul(m, r);
    matrix4i_mul(m, s);
}

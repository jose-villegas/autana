/*
 * fix3: small3dlib's (drummyfish, CC0) integer transform maths, kept
 * bit-exact including its quirks, shared by the line camera, the animation
 * transforms and the boot scene. Units are 512 to 1.0 and one full turn is
 * 512 as an angle; products are plain int32, so a caller keeps its
 * coordinates small enough not to overflow (boot_anim.h states its bound).
 *
 * Integer, so the same inputs give the same pixel on the board, on x86 and
 * in a pinned host render; an angle is one table lookup, with no libm.
 *
 * Two quirks are kept on purpose, and a test pins both: the sine table peaks
 * at 510, not FIX3_ONE, and fix3_wrap() comes out one short for a negative
 * input. Header-only, static inline and ESP-IDF-free.
 */
#pragma once

#include <stdint.h>

#define FIX3_ONE 512

typedef int32_t fix3_unit_t;

typedef struct {
    fix3_unit_t x, y, z, w;
} fix3_vec4_t;

/* Indexed [column][row], acting on column vectors. */
typedef fix3_unit_t fix3_mat4_t[4][4];

/* Rotation is Euler angles in turns, applied Z, then X, then Y. */
typedef struct {
    fix3_vec4_t translation;
    fix3_vec4_t rotation;
    fix3_vec4_t scale;
} fix3_transform_t;

#define FIX3_SIN_TABLE_LENGTH 128

/* A quarter turn of sine; the last entry is 510, not FIX3_ONE. */
static const fix3_unit_t fix3_sin_table[FIX3_SIN_TABLE_LENGTH] = {
    0,   6,   12,  18,  25,  31,  37,  43,  50,  56,  62,  68,  74,  81,  87,  93,  99,  105, 111, 118, 124, 130,
    136, 142, 148, 154, 160, 166, 172, 178, 183, 189, 195, 201, 207, 212, 218, 224, 229, 235, 240, 246, 251, 257,
    262, 268, 273, 278, 283, 289, 294, 299, 304, 309, 314, 319, 324, 328, 333, 338, 343, 347, 352, 356, 361, 365,
    370, 374, 378, 382, 386, 391, 395, 398, 402, 406, 410, 414, 417, 421, 424, 428, 431, 435, 438, 441, 444, 447,
    450, 453, 456, 459, 461, 464, 467, 469, 472, 474, 476, 478, 481, 483, 485, 487, 488, 490, 492, 494, 495, 497,
    498, 499, 501, 502, 503, 504, 505, 506, 507, 507, 508, 509, 509, 510, 510, 510, 510, 510,
};

static inline fix3_unit_t
fix3_non_zero(fix3_unit_t value) {
    return value + (value == 0);
}

/* Kept on purpose; pinned renders depend on it. */
static inline fix3_unit_t
fix3_wrap(fix3_unit_t value, fix3_unit_t mod) {
    return value >= 0 ? (value % mod) : (mod + (value % mod) - 1);
}

static inline fix3_unit_t
fix3_sin(fix3_unit_t angle) {
    fix3_unit_t x = fix3_wrap(angle, FIX3_SIN_TABLE_LENGTH * 4);
    int positive = 1;

    if (x >= FIX3_SIN_TABLE_LENGTH * 3) {
        x = FIX3_SIN_TABLE_LENGTH - (x - FIX3_SIN_TABLE_LENGTH * 3) - 1;
        positive = 0;
    } else if (x >= FIX3_SIN_TABLE_LENGTH * 2) {
        x = x - FIX3_SIN_TABLE_LENGTH * 2;
        positive = 0;
    } else if (x >= FIX3_SIN_TABLE_LENGTH) {
        x = FIX3_SIN_TABLE_LENGTH * 2 - x - 1;
    }
    return positive ? fix3_sin_table[x] : -fix3_sin_table[x];
}

static inline fix3_unit_t
fix3_cos(fix3_unit_t angle) {
    return fix3_sin(angle + FIX3_ONE / 4);
}

static inline void
fix3_vec4_init(fix3_vec4_t* v) {
    *v = (fix3_vec4_t){0, 0, 0, FIX3_ONE};
}

static inline void
fix3_transform_init(fix3_transform_t* t) {
    fix3_vec4_init(&t->translation);
    fix3_vec4_init(&t->rotation);
    t->scale = (fix3_vec4_t){FIX3_ONE, FIX3_ONE, FIX3_ONE, 0};
}

static inline void
fix3_mat4_init(fix3_mat4_t m) {
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            m[c][r] = c == r ? FIX3_ONE : 0;
        }
    }
}

static inline void
fix3_mat4_copy(fix3_mat4_t src, fix3_mat4_t dst) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            dst[c][r] = src[c][r];
        }
    }
}

/* m1 = m1 * m2. */
static inline void
fix3_mat4_mul(fix3_mat4_t m1, fix3_mat4_t m2) {
    fix3_mat4_t a;
    fix3_mat4_copy(m1, a);

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            m1[c][r] = 0;
            for (int i = 0; i < 4; i++) {
                m1[c][r] += (a[i][r] * m2[c][i]) / FIX3_ONE;
            }
        }
    }
}

/* Point times matrix, translation included; w comes back FIX3_ONE. */
static inline void
fix3_vec3_transform(fix3_vec4_t* v, fix3_mat4_t m) {
    const fix3_vec4_t in = *v;

    v->x = (in.x * m[0][0]) / FIX3_ONE + (in.y * m[0][1]) / FIX3_ONE + (in.z * m[0][2]) / FIX3_ONE + m[0][3];
    v->y = (in.x * m[1][0]) / FIX3_ONE + (in.y * m[1][1]) / FIX3_ONE + (in.z * m[1][2]) / FIX3_ONE + m[1][3];
    v->z = (in.x * m[2][0]) / FIX3_ONE + (in.y * m[2][1]) / FIX3_ONE + (in.z * m[2][2]) / FIX3_ONE + m[2][3];
    v->w = FIX3_ONE;
}

/* Divides x and y by z, scaled by the focal length; 0 is orthographic. Does
 * not check z. */
static inline void
fix3_perspective_divide(fix3_vec4_t* v, fix3_unit_t focal) {
    if (focal == 0) {
        return;
    }
    v->x = (v->x * focal) / v->z;
    v->y = (v->y * focal) / v->z;
}

static inline void
fix3_translation_matrix(fix3_unit_t x, fix3_unit_t y, fix3_unit_t z, fix3_mat4_t m) {
    fix3_mat4_init(m);
    m[0][3] = x;
    m[1][3] = y;
    m[2][3] = z;
}

static inline void
fix3_scale_matrix(fix3_unit_t x, fix3_unit_t y, fix3_unit_t z, fix3_mat4_t m) {
    fix3_mat4_init(m);
    m[0][0] = x;
    m[1][1] = y;
    m[2][2] = z;
}

/* Z, then X, then Y; each angle in turns, negated before its sine. */
static inline void
fix3_rotation_matrix(fix3_unit_t by_x, fix3_unit_t by_y, fix3_unit_t by_z, fix3_mat4_t m) {
    const fix3_unit_t sx = fix3_sin(-by_x);
    const fix3_unit_t sy = fix3_sin(-by_y);
    const fix3_unit_t sz = fix3_sin(-by_z);
    const fix3_unit_t cx = fix3_cos(-by_x);
    const fix3_unit_t cy = fix3_cos(-by_y);
    const fix3_unit_t cz = fix3_cos(-by_z);
    const fix3_unit_t s = FIX3_ONE;

    m[0][0] = (cy * cz) / s + (sy * sx * sz) / (s * s);
    m[1][0] = (cx * sz) / s;
    m[2][0] = (cy * sx * sz) / (s * s) - (cz * sy) / s;
    m[3][0] = 0;

    m[0][1] = (cz * sy * sx) / (s * s) - (cy * sz) / s;
    m[1][1] = (cx * cz) / s;
    m[2][1] = (cy * cz * sx) / (s * s) + (sy * sz) / s;
    m[3][1] = 0;

    m[0][2] = (cx * sy) / s;
    m[1][2] = -sx;
    m[2][2] = (cy * cx) / s;
    m[3][2] = 0;

    m[0][3] = 0;
    m[1][3] = 0;
    m[2][3] = 0;
    m[3][3] = FIX3_ONE;
}

/* Model to world: scale, then rotate, then translate. */
static inline void
fix3_world_matrix(fix3_transform_t world, fix3_mat4_t m) {
    fix3_mat4_t t;

    fix3_scale_matrix(world.scale.x, world.scale.y, world.scale.z, m);
    fix3_rotation_matrix(world.rotation.x, world.rotation.y, world.rotation.z, t);
    fix3_mat4_mul(m, t);
    fix3_translation_matrix(world.translation.x, world.translation.y, world.translation.z, t);
    fix3_mat4_mul(m, t);
}

/* World to camera: the inverse of the camera's own pose. */
static inline void
fix3_camera_matrix(fix3_transform_t camera, fix3_mat4_t m) {
    fix3_mat4_t r, s;

    fix3_translation_matrix(-camera.translation.x, -camera.translation.y, -camera.translation.z, m);
    fix3_rotation_matrix(camera.rotation.x, camera.rotation.y, camera.rotation.z, r);
    for (int y = 0; y < 3; y++) {
        for (int x = y + 1; x < 4; x++) {
            const fix3_unit_t tmp = r[x][y];
            r[x][y] = r[y][x];
            r[y][x] = tmp;
        }
    }
    fix3_scale_matrix(camera.scale.x, camera.scale.y, camera.scale.z, s);
    fix3_mat4_mul(m, r);
    fix3_mat4_mul(m, s);
}

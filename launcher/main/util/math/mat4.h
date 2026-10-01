/*
 * mat4: a 4x4 float matrix indexed m[row][col], acting on column vectors, so
 * a point is transformed as M * p and `mat4_mul(a, b)` applies b first. The
 * translation is column 3. Every matrix in the renderer is this one;
 * transform.h's transform_t is what callers hold. Float only, see vec3.h.
 */
#pragma once

#include "util/math/quat.h"
#include "util/math/vec3.h"

typedef struct {
    float m[4][4];
} mat4_t;

static inline mat4_t
mat4_identity(void) {
    return (mat4_t){
        {{1.0F, 0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}}};
}

/* a * b. */
static inline mat4_t
mat4_mul(mat4_t a, mat4_t b) {
    mat4_t out;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out.m[r][c] =
                (a.m[r][0] * b.m[0][c]) + (a.m[r][1] * b.m[1][c]) + (a.m[r][2] * b.m[2][c]) + (a.m[r][3] * b.m[3][c]);
        }
    }
    return out;
}

/* A point, translation included. */
static inline vec3_t
mat4_apply(const mat4_t* m, vec3_t p) {
    return (vec3_t){
        (m->m[0][0] * p.x) + (m->m[0][1] * p.y) + (m->m[0][2] * p.z) + m->m[0][3],
        (m->m[1][0] * p.x) + (m->m[1][1] * p.y) + (m->m[1][2] * p.z) + m->m[1][3],
        (m->m[2][0] * p.x) + (m->m[2][1] * p.y) + (m->m[2][2] * p.z) + m->m[2][3],
    };
}

/* Scale, then rotate, then translate. `rotation` must be unit length. */
static inline mat4_t
mat4_from_trs(vec3_t position, quat_t rotation, vec3_t scale) {
    const float x = rotation.x;
    const float y = rotation.y;
    const float z = rotation.z;
    const float w = rotation.w;
    return (mat4_t){{
        {(1.0F - (2.0F * ((y * y) + (z * z)))) * scale.x, 2.0F * ((x * y) - (z * w)) * scale.y,
         2.0F * ((x * z) + (y * w)) * scale.z, position.x},
        {2.0F * ((x * y) + (z * w)) * scale.x, (1.0F - (2.0F * ((x * x) + (z * z)))) * scale.y,
         2.0F * ((y * z) - (x * w)) * scale.z, position.y},
        {2.0F * ((x * z) - (y * w)) * scale.x, 2.0F * ((y * z) + (x * w)) * scale.y,
         (1.0F - (2.0F * ((x * x) + (y * y)))) * scale.z, position.z},
        {0.0F, 0.0F, 0.0F, 1.0F},
    }};
}

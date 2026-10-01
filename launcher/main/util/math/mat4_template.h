/*
 * mat4_template: a 4x4 matrix indexed m[row][col], acting on column vectors,
 * for one number type; see math_template.h for the macro arguments. A point
 * is transformed as M * p and `mul(a, b)` applies b first; the translation is
 * column 3.
 */
#pragma once

#include <stdbool.h>

#define MATH_DEFINE_MAT4(P, V, Q, T, OPS)                                                                              \
    typedef struct {                                                                                                   \
        T m[4][4];                                                                                                     \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* The identity matrix. */                                                                                         \
    static inline P##_t P##_identity(void) {                                                                           \
        return (P##_t){{{OPS##_one(), OPS##_zero(), OPS##_zero(), OPS##_zero()},                                       \
                        {OPS##_zero(), OPS##_one(), OPS##_zero(), OPS##_zero()},                                       \
                        {OPS##_zero(), OPS##_zero(), OPS##_one(), OPS##_zero()},                                       \
                        {OPS##_zero(), OPS##_zero(), OPS##_zero(), OPS##_one()}}};                                     \
    }                                                                                                                  \
                                                                                                                       \
    /* a * b. */                                                                                                       \
    static inline P##_t P##_mul(P##_t a, P##_t b) {                                                                    \
        P##_t out;                                                                                                     \
        for (int r = 0; r < 4; r++) {                                                                                  \
            for (int c = 0; c < 4; c++) {                                                                              \
                out.m[r][c] =                                                                                          \
                    OPS##_add(OPS##_add(OPS##_add(OPS##_mul(a.m[r][0], b.m[0][c]), OPS##_mul(a.m[r][1], b.m[1][c])),   \
                                        OPS##_mul(a.m[r][2], b.m[2][c])),                                              \
                              OPS##_mul(a.m[r][3], b.m[3][c]));                                                        \
            }                                                                                                          \
        }                                                                                                              \
        return out;                                                                                                    \
    }                                                                                                                  \
                                                                                                                       \
    /* A point, translation included. */                                                                               \
    /* M * p with w = 1: translation included, no perspective divide. `m` is read only. */                             \
    static inline V##_t P##_apply(const P##_t* m, V##_t p) {                                                           \
        return (V##_t){                                                                                                \
            OPS##_add(OPS##_add(OPS##_add(OPS##_mul(m->m[0][0], p.x), OPS##_mul(m->m[0][1], p.y)),                     \
                                OPS##_mul(m->m[0][2], p.z)),                                                           \
                      m->m[0][3]),                                                                                     \
            OPS##_add(OPS##_add(OPS##_add(OPS##_mul(m->m[1][0], p.x), OPS##_mul(m->m[1][1], p.y)),                     \
                                OPS##_mul(m->m[1][2], p.z)),                                                           \
                      m->m[1][3]),                                                                                     \
            OPS##_add(OPS##_add(OPS##_add(OPS##_mul(m->m[2][0], p.x), OPS##_mul(m->m[2][1], p.y)),                     \
                                OPS##_mul(m->m[2][2], p.z)),                                                           \
                      m->m[2][3]),                                                                                     \
        };                                                                                                             \
    }                                                                                                                  \
                                                                                                                       \
    /* Scale, then rotate, then translate. `rotation` must be unit length. */                                          \
    static inline P##_t P##_from_trs(V##_t position, Q##_t rotation, V##_t scale) {                                    \
        const T x = rotation.x;                                                                                        \
        const T y = rotation.y;                                                                                        \
        const T z = rotation.z;                                                                                        \
        const T w = rotation.w;                                                                                        \
        const T xx = OPS##_mul(x, x);                                                                                  \
        const T yy = OPS##_mul(y, y);                                                                                  \
        const T zz = OPS##_mul(z, z);                                                                                  \
        return (P##_t){{                                                                                               \
            {OPS##_mul(OPS##_sub(OPS##_one(), OPS##_mul(OPS##_two(), OPS##_add(yy, zz))), scale.x),                    \
             OPS##_mul(OPS##_mul(OPS##_two(), OPS##_sub(OPS##_mul(x, y), OPS##_mul(z, w))), scale.y),                  \
             OPS##_mul(OPS##_mul(OPS##_two(), OPS##_add(OPS##_mul(x, z), OPS##_mul(y, w))), scale.z), position.x},     \
            {OPS##_mul(OPS##_mul(OPS##_two(), OPS##_add(OPS##_mul(x, y), OPS##_mul(z, w))), scale.x),                  \
             OPS##_mul(OPS##_sub(OPS##_one(), OPS##_mul(OPS##_two(), OPS##_add(xx, zz))), scale.y),                    \
             OPS##_mul(OPS##_mul(OPS##_two(), OPS##_sub(OPS##_mul(y, z), OPS##_mul(x, w))), scale.z), position.y},     \
            {OPS##_mul(OPS##_mul(OPS##_two(), OPS##_sub(OPS##_mul(x, z), OPS##_mul(y, w))), scale.x),                  \
             OPS##_mul(OPS##_mul(OPS##_two(), OPS##_add(OPS##_mul(y, z), OPS##_mul(x, w))), scale.y),                  \
             OPS##_mul(OPS##_sub(OPS##_one(), OPS##_mul(OPS##_two(), OPS##_add(xx, yy))), scale.z), position.z},       \
            {OPS##_zero(), OPS##_zero(), OPS##_zero(), OPS##_one()},                                                   \
        }};                                                                                                            \
    }

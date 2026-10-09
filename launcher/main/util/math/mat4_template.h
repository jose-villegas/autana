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
    /* identity: m[r][c] = (r == c ? 1 : 0) */                                                                         \
    static inline P##_t P##_identity(void) {                                                                           \
        return (P##_t){{{OPS##_one(), OPS##_zero(), OPS##_zero(), OPS##_zero()},                                       \
                        {OPS##_zero(), OPS##_one(), OPS##_zero(), OPS##_zero()},                                       \
                        {OPS##_zero(), OPS##_zero(), OPS##_one(), OPS##_zero()},                                       \
                        {OPS##_zero(), OPS##_zero(), OPS##_zero(), OPS##_one()}}};                                     \
    }                                                                                                                  \
                                                                                                                       \
    /* (a * b)[r][c] = a[r][0]*b[0][c] + a[r][1]*b[1][c] + a[r][2]*b[2][c] + a[r][3]*b[3][c] */                        \
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
    /* Both inputs have bottom row (0, 0, 0, 1); b is applied first. */                                                \
    static inline P##_t P##_mul_affine(P##_t a, P##_t b) {                                                             \
        P##_t out = P##_identity();                                                                                    \
        for (int r = 0; r < 3; r++) {                                                                                  \
            for (int c = 0; c < 4; c++) {                                                                              \
                const T dot = OPS##_add(OPS##_add(OPS##_mul(a.m[r][0], b.m[0][c]), OPS##_mul(a.m[r][1], b.m[1][c])),   \
                                        OPS##_mul(a.m[r][2], b.m[2][c]));                                              \
                out.m[r][c] = c == 3 ? OPS##_add(dot, a.m[r][3]) : dot;                                                \
            }                                                                                                          \
        }                                                                                                              \
        return out;                                                                                                    \
    }                                                                                                                  \
                                                                                                                       \
    /* The affine input's 3x3 part must be invertible in this number type. */                                          \
    static inline P##_t P##_invert_affine(P##_t m) {                                                                   \
        P##_t out = P##_identity();                                                                                    \
        for (int r = 0; r < 3; r++) {                                                                                  \
            for (int c = 0; c < 3; c++) {                                                                              \
                const int r1 = (c + 1) % 3, r2 = (c + 2) % 3;                                                          \
                const int c1 = (r + 1) % 3, c2 = (r + 2) % 3;                                                          \
                out.m[r][c] = OPS##_sub(OPS##_mul(m.m[r1][c1], m.m[r2][c2]), OPS##_mul(m.m[r1][c2], m.m[r2][c1]));     \
            }                                                                                                          \
        }                                                                                                              \
        const T det = OPS##_add(OPS##_add(OPS##_mul(m.m[0][0], out.m[0][0]), OPS##_mul(m.m[0][1], out.m[1][0])),       \
                                OPS##_mul(m.m[0][2], out.m[2][0]));                                                    \
        const T inv = OPS##_div(OPS##_one(), det);                                                                     \
        for (int r = 0; r < 3; r++) {                                                                                  \
            for (int c = 0; c < 3; c++) {                                                                              \
                out.m[r][c] = OPS##_mul(out.m[r][c], inv);                                                             \
            }                                                                                                          \
            out.m[r][3] =                                                                                              \
                OPS##_neg(OPS##_add(OPS##_add(OPS##_mul(out.m[r][0], m.m[0][3]), OPS##_mul(out.m[r][1], m.m[1][3])),   \
                                    OPS##_mul(out.m[r][2], m.m[2][3])));                                               \
        }                                                                                                              \
        return out;                                                                                                    \
    }                                                                                                                  \
                                                                                                                       \
    /* M * p with w = 1, no perspective divide: for r in 0..2, */                                                      \
    /*   p'[r] = m[r][0]*px + m[r][1]*py + m[r][2]*pz + m[r][3]. `m` is read only. */                                  \
    static inline V##_t P##_apply(const P##_t* m, V##_t p) {                                                           \
        return (V##_t){                                                                                                \
            OPS##_dot3c(m->m[0][0], p.x, m->m[0][1], p.y, m->m[0][2], p.z, m->m[0][3]),                                \
            OPS##_dot3c(m->m[1][0], p.x, m->m[1][1], p.y, m->m[1][2], p.z, m->m[1][3]),                                \
            OPS##_dot3c(m->m[2][0], p.x, m->m[2][1], p.y, m->m[2][2], p.z, m->m[2][3]),                                \
        };                                                                                                             \
    }                                                                                                                  \
                                                                                                                       \
    /* M = T * R * S, scale then rotate then translate; `rotation` must be unit length. */                             \
    /* Column c of R is scaled by scale.c and the position is column 3, with R = */                                    \
    /*   1 - 2(yy + zz)   2(xy - zw)       2(xz + yw) */                                                               \
    /*   2(xy + zw)       1 - 2(xx + zz)   2(yz - xw) */                                                               \
    /*   2(xz - yw)       2(yz + xw)       1 - 2(xx + yy) */                                                           \
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

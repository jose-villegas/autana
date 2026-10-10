/*
 * vec3_template: the three-component vector for one number type; see
 * math_template.h for the macro arguments. normalize needs a square root and
 * a divide, so it is a macro of its own, for the types that have them. Its
 * swizzles are vec_swizzle_template.h's.
 */
#pragma once

#include <stdbool.h>

#include "math/linear/vec_swizzle_template.h"

#define MATH_DEFINE_VEC3(P, T, W, OPS)                                                                                 \
                                                                                                                       \
    typedef struct {                                                                                                   \
        T x, y, z;                                                                                                     \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* a + b = (ax + bx, ay + by, az + bz). Arguments are by value, so a result may reuse either. */                   \
    static inline P##_t P##_add(P##_t a, P##_t b) {                                                                    \
        return (P##_t){OPS##_add(a.x, b.x), OPS##_add(a.y, b.y), OPS##_add(a.z, b.z)};                                 \
    }                                                                                                                  \
                                                                                                                       \
    /* a - b = (ax - bx, ay - by, az - bz) */                                                                          \
    static inline P##_t P##_sub(P##_t a, P##_t b) {                                                                    \
        return (P##_t){OPS##_sub(a.x, b.x), OPS##_sub(a.y, b.y), OPS##_sub(a.z, b.z)};                                 \
    }                                                                                                                  \
                                                                                                                       \
    /* a * s = (ax*s, ay*s, az*s), in the number type's own units (Q16.16 for fixed). */                               \
    static inline P##_t P##_scale(P##_t a, T s) {                                                                      \
        return (P##_t){OPS##_mul(a.x, s), OPS##_mul(a.y, s), OPS##_mul(a.z, s)};                                       \
    }                                                                                                                  \
                                                                                                                       \
    /* dot = ax*bx + ay*by + az*bz, summed in the wide type W so integer products cannot wrap. */                      \
    static inline W P##_dot(P##_t a, P##_t b) {                                                                        \
        return OPS##_wadd(OPS##_wadd(OPS##_wmul(a.x, b.x), OPS##_wmul(a.y, b.y)), OPS##_wmul(a.z, b.z));               \
    }                                                                                                                  \
                                                                                                                       \
    /* equal = (ax == bx && ay == by && az == bz), exact, no tolerance. */                                             \
    static inline bool P##_equal(P##_t a, P##_t b) { return a.x == b.x && a.y == b.y && a.z == b.z; }                  \
                                                                                                                       \
    /* Cross product: x × y = z */                                                                                     \
    static inline P##_t P##_cross(P##_t a, P##_t b) {                                                                  \
        return (P##_t){OPS##_sub(OPS##_mul(a.y, b.z), OPS##_mul(a.z, b.y)),                                            \
                       OPS##_sub(OPS##_mul(a.z, b.x), OPS##_mul(a.x, b.z)),                                            \
                       OPS##_sub(OPS##_mul(a.x, b.y), OPS##_mul(a.y, b.x))};                                           \
    }                                                                                                                  \
                                                                                                                       \
    /* P_c1c2c3(v) = (v.c1, v.c2, v.c3), each c one of x, y, z. */                                                     \
    MATH_DEFINE_SWIZZLE3(P, P, 3)

#define MATH_DEFINE_VEC3_NORMALIZE(P, T, OPS)                                                                          \
    /* normalize = a * (1 / sqrt(dot(a, a))). Precondition: `a` is not the zero vector. */                             \
    static inline P##_t P##_normalize(P##_t a) {                                                                       \
        return P##_scale(a, OPS##_div(OPS##_one(), OPS##_sqrt(P##_dot(a, a))));                                        \
    }

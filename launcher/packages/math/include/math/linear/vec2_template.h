/*
 * vec2_template: the two-component vector for one number type; see
 * docs/math/Conventions.md#templates for the macro arguments. Its swizzles are
 * vec_swizzle_template.h's. No up/down: integer screen pixels have y down.
 */
#pragma once

#include <stdbool.h>

#include "math/linear/constant_template.h"

#include "math/linear/vec_ops_template.h"
#include "math/linear/vec_swizzle_template.h"

#define MATH_DEFINE_VEC2(P, T, W, OPS)                                                                                 \
                                                                                                                       \
    typedef struct {                                                                                                   \
        T x, y;                                                                                                        \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* zero = (0, 0). */                                                                                               \
    MATH_DEFINE_CONSTANT(P, zero, OPS##_zero(), OPS##_zero())                                                          \
                                                                                                                       \
    /* one = (1, 1). */                                                                                                \
    MATH_DEFINE_CONSTANT(P, one, OPS##_one(), OPS##_one())                                                             \
                                                                                                                       \
    /* unit_x = (1, 0). */                                                                                             \
    MATH_DEFINE_CONSTANT(P, unit_x, OPS##_one(), OPS##_zero())                                                         \
                                                                                                                       \
    /* right = (1, 0). */                                                                                              \
    MATH_DEFINE_CONSTANT_ALIAS(P, right, unit_x)                                                                       \
                                                                                                                       \
    /* left = (-1, 0). */                                                                                              \
    MATH_DEFINE_CONSTANT(P, left, OPS##_neg(OPS##_one()), OPS##_zero())                                                \
                                                                                                                       \
    /* unit_y = (0, 1). */                                                                                             \
    MATH_DEFINE_CONSTANT(P, unit_y, OPS##_zero(), OPS##_one())                                                         \
                                                                                                                       \
    MATH_DEFINE_VEC_OPS(P, OPS, 2)                                                                                     \
                                                                                                                       \
    /* a * s = (ax*s, ay*s), in the number type's own units (Q16.16 for fixed). */                                     \
    static inline P##_t P##_scale(P##_t a, T s) { return (P##_t){OPS##_mul(a.x, s), OPS##_mul(a.y, s)}; }              \
                                                                                                                       \
    /* dot = ax*bx + ay*by, summed in the wide type W so integer products cannot wrap. */                              \
    static inline W P##_dot(P##_t a, P##_t b) { return OPS##_wadd(OPS##_wmul(a.x, b.x), OPS##_wmul(a.y, b.y)); }       \
                                                                                                                       \
    /* equal = (ax == bx && ay == by), exact, no tolerance. */                                                         \
    static inline bool P##_equal(P##_t a, P##_t b) { return a.x == b.x && a.y == b.y; }                                \
                                                                                                                       \
    /* P_c1c2(v) = (v.c1, v.c2), each c one of x, y. */                                                                \
    MATH_DEFINE_SWIZZLE2(P, P, 2)

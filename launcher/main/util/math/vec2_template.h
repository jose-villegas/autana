/*
 * vec2_template: the two-component vector for one number type; see
 * math_template.h for the macro arguments. Its swizzles are
 * vec_swizzle_template.h's.
 */
#pragma once

#include <stdbool.h>

#include "util/math/vec_swizzle_template.h"

#define MATH_DEFINE_VEC2(P, T, W, OPS)                                                                                 \
                                                                                                                       \
    typedef struct {                                                                                                   \
        T x, y;                                                                                                        \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* a + b = (ax + bx, ay + by). Arguments are by value, so a result may reuse either. */                            \
    static inline P##_t P##_add(P##_t a, P##_t b) { return (P##_t){OPS##_add(a.x, b.x), OPS##_add(a.y, b.y)}; }        \
                                                                                                                       \
    /* a - b = (ax - bx, ay - by) */                                                                                   \
    static inline P##_t P##_sub(P##_t a, P##_t b) { return (P##_t){OPS##_sub(a.x, b.x), OPS##_sub(a.y, b.y)}; }        \
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

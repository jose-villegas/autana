/*
 * vec2_template: the two-component vector for one number type; see
 * math_template.h for the macro arguments. Its swizzles are
 * vec_swizzle_template.h's.
 */
#pragma once

#include <stdbool.h>

#include "math/linear/vec_swizzle_template.h"

#define MATH_VEC2_EACH2(P, F, a, b)           ((P##_t){F((a).x, (b).x), F((a).y, (b).y)})
#define MATH_VEC2_EACH3(P, F, a, b, c)        ((P##_t){F((a).x, (b).x, (c).x), F((a).y, (b).y, (c).y)})
#define MATH_VEC2_EACH2_ARGS(P, F, a, b, ...) ((P##_t){F((a).x, (b).x, __VA_ARGS__), F((a).y, (b).y, __VA_ARGS__)})

#define MATH_DEFINE_VEC2(P, T, W, OPS)                                                                                 \
                                                                                                                       \
    typedef struct {                                                                                                   \
        T x, y;                                                                                                        \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* a + b = (ax + bx, ay + by). Arguments are by value, so a result may reuse either. */                            \
    static inline P##_t P##_add(P##_t a, P##_t b) { return MATH_VEC2_EACH2(P, OPS##_add, a, b); }                      \
                                                                                                                       \
    /* a - b = (ax - bx, ay - by) */                                                                                   \
    static inline P##_t P##_sub(P##_t a, P##_t b) { return MATH_VEC2_EACH2(P, OPS##_sub, a, b); }                      \
                                                                                                                       \
    /* min = (min(ax, bx), min(ay, by)). */                                                                            \
    static inline P##_t P##_min(P##_t a, P##_t b) { return MATH_VEC2_EACH2(P, OPS##_min, a, b); }                      \
                                                                                                                       \
    /* max = (max(ax, bx), max(ay, by)). */                                                                            \
    static inline P##_t P##_max(P##_t a, P##_t b) { return MATH_VEC2_EACH2(P, OPS##_max, a, b); }                      \
                                                                                                                       \
    /* clamp = (clamp(vx, lox, hix), clamp(vy, loy, hiy)). */                                                          \
    static inline P##_t P##_clamp(P##_t v, P##_t lo, P##_t hi) { return MATH_VEC2_EACH3(P, OPS##_clamp, v, lo, hi); }  \
                                                                                                                       \
    /* repeat = (repeat(vx, lenx), repeat(vy, leny)). */                                                               \
    static inline P##_t P##_repeat(P##_t v, P##_t len) { return MATH_VEC2_EACH2(P, OPS##_repeat, v, len); }            \
                                                                                                                       \
    /* wrap = (wrap(vx, lox, hix), wrap(vy, loy, hiy)). */                                                             \
    static inline P##_t P##_wrap(P##_t v, P##_t lo, P##_t hi) { return MATH_VEC2_EACH3(P, OPS##_wrap, v, lo, hi); }    \
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

#define MATH_DEFINE_VEC2_LERP(P, T, OPS)                                                                               \
    /* lerp = a + (b - a) * t, per component. */                                                                       \
    static inline P##_t P##_lerp(P##_t a, P##_t b, T t) { return MATH_VEC2_EACH2_ARGS(P, OPS##_lerp, a, b, t); }

#define MATH_DEFINE_VEC2_LERP_RATIO(P, OPS)                                                                            \
    /* lerp = a + (b - a) * t / n; truncates toward a, per component. */                                               \
    static inline P##_t P##_lerp(P##_t a, P##_t b, int32_t t, int32_t n) {                                             \
        return MATH_VEC2_EACH2_ARGS(P, OPS##_lerp, a, b, t, n);                                                        \
    }

/*
 * vec3_template: the three-component vector for one number type; see
 * math_template.h for the macro arguments. normalize needs a square root and
 * a divide, so it is a macro of its own, for the types that have them. Its
 * swizzles are vec_swizzle_template.h's.
 */
#pragma once

#include <stdbool.h>

#include "math/linear/vec_swizzle_template.h"

#define MATH_VEC3_EACH2(P, F, a, b)    ((P##_t){F((a).x, (b).x), F((a).y, (b).y), F((a).z, (b).z)})
#define MATH_VEC3_EACH3(P, F, a, b, c) ((P##_t){F((a).x, (b).x, (c).x), F((a).y, (b).y, (c).y), F((a).z, (b).z, (c).z)})
#define MATH_VEC3_EACH2_ARGS(P, F, a, b, ...)                                                                          \
    ((P##_t){F((a).x, (b).x, __VA_ARGS__), F((a).y, (b).y, __VA_ARGS__), F((a).z, (b).z, __VA_ARGS__)})

#define MATH_DEFINE_VEC3(P, T, W, OPS)                                                                                 \
                                                                                                                       \
    typedef struct {                                                                                                   \
        T x, y, z;                                                                                                     \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* a + b = (ax + bx, ay + by, az + bz). Arguments are by value, so a result may reuse either. */                   \
    static inline P##_t P##_add(P##_t a, P##_t b) { return MATH_VEC3_EACH2(P, OPS##_add, a, b); }                      \
                                                                                                                       \
    /* a - b = (ax - bx, ay - by, az - bz) */                                                                          \
    static inline P##_t P##_sub(P##_t a, P##_t b) { return MATH_VEC3_EACH2(P, OPS##_sub, a, b); }                      \
                                                                                                                       \
    /* min = (min(ax, bx), min(ay, by), min(az, bz)). */                                                               \
    static inline P##_t P##_min(P##_t a, P##_t b) { return MATH_VEC3_EACH2(P, OPS##_min, a, b); }                      \
                                                                                                                       \
    /* max = (max(ax, bx), max(ay, by), max(az, bz)). */                                                               \
    static inline P##_t P##_max(P##_t a, P##_t b) { return MATH_VEC3_EACH2(P, OPS##_max, a, b); }                      \
                                                                                                                       \
    /* clamp = (clamp(vx, lox, hix), clamp(vy, loy, hiy), clamp(vz, loz, hiz)). */                                     \
    static inline P##_t P##_clamp(P##_t v, P##_t lo, P##_t hi) { return MATH_VEC3_EACH3(P, OPS##_clamp, v, lo, hi); }  \
                                                                                                                       \
    /* repeat = (repeat(vx, lenx), repeat(vy, leny), repeat(vz, lenz)). */                                             \
    static inline P##_t P##_repeat(P##_t v, P##_t len) { return MATH_VEC3_EACH2(P, OPS##_repeat, v, len); }            \
                                                                                                                       \
    /* wrap = (wrap(vx, lox, hix), wrap(vy, loy, hiy), wrap(vz, loz, hiz)). */                                         \
    static inline P##_t P##_wrap(P##_t v, P##_t lo, P##_t hi) { return MATH_VEC3_EACH3(P, OPS##_wrap, v, lo, hi); }    \
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
    /* Right-handed: a x b = (ay*bz - az*by, az*bx - ax*bz, ax*by - ay*bx) */                                          \
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

#define MATH_DEFINE_VEC3_LERP(P, T, OPS)                                                                               \
    /* lerp = a + (b - a) * t, per component. */                                                                       \
    static inline P##_t P##_lerp(P##_t a, P##_t b, T t) { return MATH_VEC3_EACH2_ARGS(P, OPS##_lerp, a, b, t); }

#define MATH_DEFINE_VEC3_LERP_RATIO(P, OPS)                                                                            \
    /* lerp = a + (b - a) * t / n; truncates toward a, per component. */                                               \
    static inline P##_t P##_lerp(P##_t a, P##_t b, int32_t t, int32_t n) {                                             \
        return MATH_VEC3_EACH2_ARGS(P, OPS##_lerp, a, b, t, n);                                                        \
    }

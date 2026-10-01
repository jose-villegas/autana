/*
 * vec2_template: the two-component vector for one number type; see
 * math_template.h for the macro arguments.
 */
#pragma once

#include <stdbool.h>

#define MATH_DEFINE_VEC2(P, T, W, OPS)                                                                                 \
                                                                                                                       \
    typedef struct {                                                                                                   \
        T x, y;                                                                                                        \
    } P##_t;                                                                                                           \
                                                                                                                       \
    static inline P##_t P##_add(P##_t a, P##_t b) { return (P##_t){OPS##_add(a.x, b.x), OPS##_add(a.y, b.y)}; }        \
                                                                                                                       \
    static inline P##_t P##_sub(P##_t a, P##_t b) { return (P##_t){OPS##_sub(a.x, b.x), OPS##_sub(a.y, b.y)}; }        \
                                                                                                                       \
    static inline P##_t P##_scale(P##_t a, T s) { return (P##_t){OPS##_mul(a.x, s), OPS##_mul(a.y, s)}; }              \
                                                                                                                       \
    static inline W P##_dot(P##_t a, P##_t b) { return OPS##_wadd(OPS##_wmul(a.x, b.x), OPS##_wmul(a.y, b.y)); }       \
                                                                                                                       \
    static inline bool P##_equal(P##_t a, P##_t b) { return a.x == b.x && a.y == b.y; }

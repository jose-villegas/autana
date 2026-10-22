/*
 * circle_template: closed discs for one number type; see
 * docs/math/Conventions.md#templates for macro arguments. R is the rect prefix.
 * Radii are nonnegative; rect queries require finite, nonempty bounds.
 * EDGE is one cell for integers, zero for real coordinates; arithmetic must fit.
 */
#pragma once
#include "math/geometry/ball_template.h"

#define MATH_DEFINE_CIRCLE(P, V, R, T, OPS, EDGE)                                                                      \
    MATH_DEFINE_BALL(P, V, T, OPS)                                                                                     \
    /* overlaps_rect = |clamp(c, lo, hi)-c|^2 <= r^2; hi = right/bottom (-1 for int, closure for real) (Arvo 1990). */ \
    static inline bool P##_overlaps_rect(P##_t c, R##_t rect) {                                                        \
        const V##_t lo = {rect.x, rect.y};                                                                             \
        const V##_t hi = {R##_right(rect) - EDGE, R##_bottom(rect) - EDGE};                                            \
        return P##_contains_point(c, V##_clamp(c.centre, lo, hi));                                                     \
    }                                                                                                                  \
    /* bounds = (cx-r, cy-r, 2r+1, 2r+1) cells for int, (cx-r, cy-r, 2r, 2r) for real. */                              \
    static inline R##_t P##_bounds(P##_t c) {                                                                          \
        const T size = OPS##_add(c.radius, c.radius) + EDGE;                                                           \
        return (R##_t){OPS##_sub(c.centre.x, c.radius), OPS##_sub(c.centre.y, c.radius), size, size};                  \
    }

#define MATH_DEFINE_CIRCLE_REAL(P, V, R, T, OPS)                                                                       \
    MATH_DEFINE_BALL_REAL(P, V, T, OPS)                                                                                \
    /* from_rect = (rect centre, length(rect half extent)). */                                                         \
    static inline P##_t P##_from_rect(R##_t rect) {                                                                    \
        const V##_t e = {OPS##_mul(rect.w, OPS##_half()), OPS##_mul(rect.h, OPS##_half())};                            \
        const V##_t corner = {rect.x, rect.y};                                                                         \
        return (P##_t){V##_add(corner, e), OPS##_sqrt(V##_dot(e, e))};                                                 \
    }

/*
 * sphere_template: closed spheres for a real number type; see
 * docs/math/Conventions.md#templates for macro arguments. B is the box prefix.
 * Radii are nonnegative; boxes for from_box and overlaps_box are finite and nonempty.
 */
#pragma once
#include "math/geometry/ball_template.h"

#define MATH_DEFINE_SPHERE(P, V, B, T, OPS)                                                                            \
    MATH_DEFINE_BALL(P, V, T, OPS)                                                                                     \
    MATH_DEFINE_BALL_REAL(P, V, T, OPS)                                                                                \
    /* from_box = (box centre, length(box half extent)). */                                                            \
    static inline P##_t P##_from_box(B##_t b) {                                                                        \
        const V##_t e = B##_half_extent(b);                                                                            \
        return (P##_t){B##_centre(b), OPS##_sqrt(V##_dot(e, e))};                                                      \
    }                                                                                                                  \
                                                                                                                       \
    /* overlaps_box = |closest_point(box, centre) - centre|^2 <= radius^2 (Arvo 1990). */                              \
    static inline bool P##_overlaps_box(P##_t s, B##_t b) {                                                            \
        return P##_contains_point(s, B##_closest_point(b, s.centre));                                                  \
    }

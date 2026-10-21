/*
 * sphere_template: closed spheres for a real number type; see
 * docs/math/Conventions.md#templates for macro arguments. B is the box prefix.
 * Radii are nonnegative; boxes for from_box and overlaps_box are finite and nonempty.
 */
#pragma once
#include <stdbool.h>

#define MATH_DEFINE_SPHERE(P, V, B, T, OPS)                                                                            \
    typedef struct {                                                                                                   \
        V##_t centre;                                                                                                  \
        T radius;                                                                                                      \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* from_box = (box centre, length(box half extent)). */                                                            \
    static inline P##_t P##_from_box(B##_t b) {                                                                        \
        const V##_t e = B##_half_extent(b);                                                                            \
        return (P##_t){B##_centre(b), OPS##_sqrt(V##_dot(e, e))};                                                      \
    }                                                                                                                  \
                                                                                                                       \
    /* contains_point = dot(p - centre, p - centre) <= radius^2. */                                                    \
    static inline bool P##_contains_point(P##_t s, V##_t p) {                                                          \
        const V##_t d = V##_sub(p, s.centre);                                                                          \
        return V##_dot(d, d) <= OPS##_mul(s.radius, s.radius);                                                         \
    }                                                                                                                  \
                                                                                                                       \
    /* overlaps = |a.centre - b.centre|^2 <= (a.radius + b.radius)^2. */                                               \
    static inline bool P##_overlaps(P##_t a, P##_t b) {                                                                \
        const V##_t d = V##_sub(a.centre, b.centre);                                                                   \
        const T r = OPS##_add(a.radius, b.radius);                                                                     \
        return V##_dot(d, d) <= OPS##_mul(r, r);                                                                       \
    }                                                                                                                  \
                                                                                                                       \
    /* overlaps_box = |closest_point(box, centre) - centre|^2 <= radius^2 (Arvo 1990). */                              \
    static inline bool P##_overlaps_box(P##_t s, B##_t b) {                                                            \
        return P##_contains_point(s, B##_closest_point(b, s.centre));                                                  \
    }                                                                                                                  \
                                                                                                                       \
    /* Finite endpoints and sphere values required; first entry in [0,1], with inside starts at t = 0. */              \
    static inline bool P##_segment_hit(P##_t s, V##_t a, V##_t c, T* t) {                                              \
        const V##_t m = V##_sub(a, s.centre), v = V##_sub(c, a);                                                       \
        const T offset = OPS##_sub(V##_dot(m, m), OPS##_mul(s.radius, s.radius));                                      \
        if (offset <= OPS##_zero()) {                                                                                  \
            *t = OPS##_zero();                                                                                         \
            return true;                                                                                               \
        }                                                                                                              \
        const T length = V##_dot(v, v), projection = V##_dot(m, v);                                                    \
        if (length == OPS##_zero() || projection >= OPS##_zero()) {                                                    \
            return false;                                                                                              \
        }                                                                                                              \
        const V##_t perpendicular = V##_sub(m, V##_scale(v, OPS##_div(projection, length)));                           \
        const T discriminant = OPS##_sub(OPS##_mul(s.radius, s.radius), V##_dot(perpendicular, perpendicular));        \
        if (discriminant < OPS##_zero()) {                                                                             \
            return false;                                                                                              \
        }                                                                                                              \
        const T entry =                                                                                                \
            OPS##_div(OPS##_sub(OPS##_neg(projection), OPS##_sqrt(OPS##_mul(length, discriminant))), length);          \
        if (entry < OPS##_zero() || entry > OPS##_one()) {                                                             \
            return false;                                                                                              \
        }                                                                                                              \
        *t = entry;                                                                                                    \
        return true;                                                                                                   \
    }

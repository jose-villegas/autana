/*
 * circle_template: closed discs for one number type; see
 * docs/math/Conventions.md#templates for macro arguments. R is the rect prefix.
 * Radii are nonnegative; rect queries require finite, nonempty bounds.
 * EDGE is one cell for integers, zero for real coordinates; arithmetic must fit.
 */
#pragma once
#include <stdbool.h>

#define MATH_DEFINE_CIRCLE(P, V, R, T, OPS, EDGE)                                                                      \
    typedef struct {                                                                                                   \
        V##_t centre;                                                                                                  \
        T radius;                                                                                                      \
    } P##_t;                                                                                                           \
    /* contains_point = dot(p - centre, p - centre) <= radius^2. */                                                    \
    static inline bool P##_contains_point(P##_t c, V##_t p) {                                                          \
        const V##_t d = V##_sub(p, c.centre);                                                                          \
        return V##_dot(d, d) <= OPS##_wmul(c.radius, c.radius);                                                        \
    }                                                                                                                  \
    /* overlaps = |a.centre - b.centre|^2 <= (a.radius + b.radius)^2. */                                               \
    static inline bool P##_overlaps(P##_t a, P##_t b) {                                                                \
        const V##_t d = V##_sub(a.centre, b.centre);                                                                   \
        const T r = OPS##_add(a.radius, b.radius);                                                                     \
        return V##_dot(d, d) <= OPS##_wmul(r, r);                                                                      \
    }                                                                                                                  \
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
    /* from_rect = (rect centre, length(rect half extent)). */                                                         \
    static inline P##_t P##_from_rect(R##_t rect) {                                                                    \
        const V##_t e = {OPS##_mul(rect.w, OPS##_half()), OPS##_mul(rect.h, OPS##_half())};                            \
        const V##_t corner = {rect.x, rect.y};                                                                         \
        return (P##_t){V##_add(corner, e), OPS##_sqrt(V##_dot(e, e))};                                                 \
    }                                                                                                                  \
    /* closest_point = p inside, else centre + (p-centre)*radius/|p-centre|. */                                        \
    static inline V##_t P##_closest_point(P##_t c, V##_t p) {                                                          \
        if (P##_contains_point(c, p)) {                                                                                \
            return p;                                                                                                  \
        }                                                                                                              \
        const V##_t d = V##_sub(p, c.centre);                                                                          \
        return V##_add(c.centre, V##_scale(d, OPS##_div(c.radius, OPS##_sqrt(V##_dot(d, d)))));                        \
    }                                                                                                                  \
    /* t = (-B-sqrt(A*D))/A (Haines et al. 2019); finite values, first entry in [0,1], inside t = 0. */                \
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

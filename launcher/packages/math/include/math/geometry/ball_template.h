/*
 * ball_template: closed balls in any dimension for one number type; see
 * docs/math/Conventions.md#templates for macro arguments.
 * Radii are nonnegative; arithmetic must fit, and real values must be finite.
 */
#pragma once
#include <stdbool.h>

#define MATH_DEFINE_BALL(P, V, T, OPS)                                                                                 \
    typedef struct {                                                                                                   \
        V##_t centre;                                                                                                  \
        T radius;                                                                                                      \
    } P##_t;                                                                                                           \
    /* contains_point = |p-c|^2 <= r^2. */                                                                             \
    static inline bool P##_contains_point(P##_t c, V##_t p) {                                                          \
        const V##_t d = V##_sub(p, c.centre);                                                                          \
        return V##_dot(d, d) <= OPS##_wmul(c.radius, c.radius);                                                        \
    }                                                                                                                  \
    /* overlaps = |ca-cb|^2 <= (ra+rb)^2. */                                                                           \
    static inline bool P##_overlaps(P##_t a, P##_t b) {                                                                \
        const V##_t d = V##_sub(a.centre, b.centre);                                                                   \
        const T r = OPS##_add(a.radius, b.radius);                                                                     \
        return V##_dot(d, d) <= OPS##_wmul(r, r);                                                                      \
    }

#define MATH_DEFINE_BALL_REAL(P, V, T, OPS)                                                                            \
    /* closest_point = p inside, else c + (p-c)*r/|p-c|. */                                                            \
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

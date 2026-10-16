/* affine2_template: a 2D affine map for one number type; see docs/math/README.md for the macro arguments. */
#pragma once

#include <stdbool.h>

#define MATH_DEFINE_AFFINE2_RECT(P, V, R, OPS, SUFFIX, MAP)                                                            \
    /* bbox = min/max of the four mapped corners; exact for axis-preserving maps. */                                   \
    static inline R##_t P##_##SUFFIX(P##_t t, R##_t r) {                                                               \
        const V##_t corners[] = {                                                                                      \
            P##_##MAP(t, (V##_t){r.x, r.y}),                                                                           \
            P##_##MAP(t, (V##_t){OPS##_add(r.x, r.w), r.y}),                                                           \
            P##_##MAP(t, (V##_t){r.x, OPS##_add(r.y, r.h)}),                                                           \
            P##_##MAP(t, (V##_t){OPS##_add(r.x, r.w), OPS##_add(r.y, r.h)}),                                           \
        };                                                                                                             \
        V##_t lo = corners[0], hi = corners[0];                                                                        \
        for (int i = 1; i < 4; i++) {                                                                                  \
            lo = V##_min(lo, corners[i]);                                                                              \
            hi = V##_max(hi, corners[i]);                                                                              \
        }                                                                                                              \
        return (R##_t){lo.x, lo.y, OPS##_sub(hi.x, lo.x), OPS##_sub(hi.y, lo.y)};                                      \
    }

#define MATH_DEFINE_AFFINE2(P, V, R, T, OPS)                                                                           \
    /* p' = x_axis * p.x + y_axis * p.y + origin. */                                                                   \
    typedef struct {                                                                                                   \
        V##_t x_axis, y_axis, origin;                                                                                  \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* identity: x_axis = (1,0), y_axis = (0,1), origin = (0,0). */                                                    \
    static inline P##_t P##_identity(void) {                                                                           \
        return (P##_t){{OPS##_one(), OPS##_zero()}, {OPS##_zero(), OPS##_one()}, {OPS##_zero(), OPS##_zero()}};        \
    }                                                                                                                  \
                                                                                                                       \
    /* L * v = x_axis * v.x + y_axis * v.y. */                                                                         \
    static inline V##_t P##_map_vector(P##_t t, V##_t v) {                                                             \
        return (V##_t){OPS##_add(OPS##_mul(t.x_axis.x, v.x), OPS##_mul(t.y_axis.x, v.y)),                              \
                       OPS##_add(OPS##_mul(t.x_axis.y, v.x), OPS##_mul(t.y_axis.y, v.y))};                             \
    }                                                                                                                  \
                                                                                                                       \
    /* t(p) = L * p + origin. */                                                                                       \
    static inline V##_t P##_map_point(P##_t t, V##_t p) { return V##_add(P##_map_vector(t, p), t.origin); }            \
                                                                                                                       \
    /* a * b = (L_a * b.x_axis, L_a * b.y_axis, L_a * b.origin + a.origin); b first. */                                \
    static inline P##_t P##_mul(P##_t a, P##_t b) {                                                                    \
        return (P##_t){P##_map_vector(a, b.x_axis), P##_map_vector(a, b.y_axis), P##_map_point(a, b.origin)};          \
    }                                                                                                                  \
                                                                                                                       \
    /* det = x_axis.x * y_axis.y - x_axis.y * y_axis.x. */                                                             \
    static inline T P##_determinant(P##_t t) {                                                                         \
        return OPS##_sub(OPS##_mul(t.x_axis.x, t.y_axis.y), OPS##_mul(t.x_axis.y, t.y_axis.x));                        \
    }                                                                                                                  \
                                                                                                                       \
    /* L^-1 = [d,-c;-b,a] / det, origin' = -(L^-1 * origin); det == 0 leaves out untouched. */                         \
    static inline bool P##_invert(P##_t t, P##_t* out) {                                                               \
        const T det = P##_determinant(t);                                                                              \
        if (det == OPS##_zero()) {                                                                                     \
            return false;                                                                                              \
        }                                                                                                              \
        P##_t inv = P##_identity();                                                                                    \
        inv.x_axis.x = OPS##_div(t.y_axis.y, det);                                                                     \
        inv.x_axis.y = OPS##_div(OPS##_neg(t.x_axis.y), det);                                                          \
        inv.y_axis.x = OPS##_div(OPS##_neg(t.y_axis.x), det);                                                          \
        inv.y_axis.y = OPS##_div(t.x_axis.x, det);                                                                     \
        const V##_t offset = P##_map_vector(inv, t.origin);                                                            \
        inv.origin = (V##_t){OPS##_neg(offset.x), OPS##_neg(offset.y)};                                                \
        *out = inv;                                                                                                    \
        return true;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    MATH_DEFINE_AFFINE2_RECT(P, V, R, OPS, map_rect, map_point)                                                        \
                                                                                                                       \
    /* equal = exact equality of both axes and the origin. */                                                          \
    static inline bool P##_equal(P##_t a, P##_t b) {                                                                   \
        return V##_equal(a.x_axis, b.x_axis) && V##_equal(a.y_axis, b.y_axis) && V##_equal(a.origin, b.origin);        \
    }

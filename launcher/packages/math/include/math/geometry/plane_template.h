/*
 * plane_template: oriented planes for one number type; see docs/math/README.md
 * for macro arguments. Inside means dot(normal, point) + d >= 0.
 * Frustum extraction follows Gribb and Hartmann<sup>[[72]](../../../../../../docs/Citations.md#72)</sup>:
 * z supplies the w row, and the screen rectangle sets the sides. The map is affine.
 */
#pragma once

enum {
    PLANE_FRUSTUM_NEAR,
    PLANE_FRUSTUM_LEFT,
    PLANE_FRUSTUM_RIGHT,
    PLANE_FRUSTUM_TOP,
    PLANE_FRUSTUM_BOTTOM,
    PLANE_FRUSTUM_COUNT,
};

#define MATH_DEFINE_PLANE(P, V, V2, R, M, T, OPS)                                                                      \
    typedef struct {                                                                                                   \
        V##_t normal;                                                                                                  \
        T d;                                                                                                           \
    } P##_t;                                                                                                           \
                                                                                                                       \
    static inline P##_t P##_from_normal_point(V##_t normal, V##_t point) {                                             \
        return (P##_t){normal, OPS##_neg(V##_dot(normal, point))};                                                     \
    }                                                                                                                  \
                                                                                                                       \
    static inline P##_t P##_from_points(V##_t a, V##_t b, V##_t c) {                                                   \
        return P##_from_normal_point(V##_cross(V##_sub(b, a), V##_sub(c, a)), a);                                      \
    }                                                                                                                  \
                                                                                                                       \
    static inline T P##_distance(P##_t plane, V##_t point) {                                                           \
        return OPS##_add(V##_dot(plane.normal, point), plane.d);                                                       \
    }                                                                                                                  \
                                                                                                                       \
    /* A nonzero normal is required, as for vector normalization. */                                                   \
    static inline P##_t P##_normalize(P##_t plane) {                                                                   \
        const T scale = OPS##_div(OPS##_one(), OPS##_sqrt(V##_dot(plane.normal, plane.normal)));                       \
        return (P##_t){V##_scale(plane.normal, scale), OPS##_mul(plane.d, scale)};                                     \
    }                                                                                                                  \
                                                                                                                       \
    static inline void P##_frustum(const M##_t* m, T near_z, V2##_t center, R##_t screen,                              \
                                   P##_t out[PLANE_FRUSTUM_COUNT]) {                                                   \
        const T zero = OPS##_zero(), one = OPS##_one();                                                                \
        const T in_lens[PLANE_FRUSTUM_COUNT][4] = {                                                                    \
            {zero, zero, one, OPS##_neg(near_z)},                                                                      \
            {one, zero, OPS##_sub(center.x, screen.x), zero},                                                          \
            {OPS##_neg(one), zero, OPS##_sub(R##_right(screen), center.x), zero},                                      \
            {zero, one, OPS##_sub(center.y, screen.y), zero},                                                          \
            {zero, OPS##_neg(one), OPS##_sub(R##_bottom(screen), center.y), zero},                                     \
        };                                                                                                             \
        for (int p = 0; p < PLANE_FRUSTUM_COUNT; p++) {                                                                \
            T coefficients[4];                                                                                         \
            for (int j = 0; j < 4; j++) {                                                                              \
                coefficients[j] =                                                                                      \
                    OPS##_add(OPS##_add(OPS##_mul(in_lens[p][0], m->m[0][j]), OPS##_mul(in_lens[p][1], m->m[1][j])),   \
                              OPS##_mul(in_lens[p][2], m->m[2][j]));                                                   \
            }                                                                                                          \
            out[p] = (P##_t){{coefficients[0], coefficients[1], coefficients[2]},                                      \
                             OPS##_add(coefficients[3], in_lens[p][3])};                                               \
        }                                                                                                              \
    }

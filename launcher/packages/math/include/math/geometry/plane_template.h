/*
 * plane_template: oriented planes for one number type; see docs/math/README.md
 * for macro arguments. Inside means dot(normal, point) + d >= 0.
 * Frustum extraction follows Gribb and Hartmann 2001:
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

#define MATH_DEFINE_PLANE(P, V, R, M, L, T, OPS)                                                                       \
    typedef struct {                                                                                                   \
        V##_t normal;                                                                                                  \
        T d;                                                                                                           \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* plane through point = (normal, -dot(normal, point)). */                                                         \
    static inline P##_t P##_from_normal_point(V##_t normal, V##_t point) {                                             \
        return (P##_t){normal, OPS##_neg(V##_dot(normal, point))};                                                     \
    }                                                                                                                  \
                                                                                                                       \
    /* normal = cross(b - a, c - a), d = -dot(normal, a); inside by winding, unnormalized. */                          \
    static inline P##_t P##_from_points(V##_t a, V##_t b, V##_t c) {                                                   \
        return P##_from_normal_point(V##_cross(V##_sub(b, a), V##_sub(c, a)), a);                                      \
    }                                                                                                                  \
                                                                                                                       \
    /* distance = dot(normal, point) + d, scaled by |normal| unless normalized. */                                     \
    static inline T P##_distance(P##_t plane, V##_t point) {                                                           \
        return OPS##_add(V##_dot(plane.normal, point), plane.d);                                                       \
    }                                                                                                                  \
                                                                                                                       \
    /* normalize = (normal, d) / |normal|. Precondition: dot(normal, normal) is nonzero and representable. */          \
    static inline P##_t P##_normalize(P##_t plane) {                                                                   \
        const T scale = OPS##_div(OPS##_one(), OPS##_sqrt(V##_dot(plane.normal, plane.normal)));                       \
        return (P##_t){V##_scale(plane.normal, scale), OPS##_mul(plane.d, scale)};                                     \
    }                                                                                                                  \
                                                                                                                       \
    /* The plane a x + b y + c z + e >= 0 of m's target, read in m's source space: */                                  \
    /* normal_j = a m[0][j] + b m[1][j] + c m[2][j], d = a m[0][3] + b m[1][3] + c m[2][3] + e. */                     \
    static inline __attribute__((always_inline)) P##_t P##_from_rows(const M##_t* m, T a, T b, T c, T e) {             \
        T w[4];                                                                                                        \
        for (int j = 0; j < 4; j++) {                                                                                  \
            w[j] = OPS##_add(OPS##_add(OPS##_mul(a, m->m[0][j]), OPS##_mul(b, m->m[1][j])), OPS##_mul(c, m->m[2][j])); \
        }                                                                                                              \
        return (P##_t){{w[0], w[1], w[2]}, OPS##_add(w[3], e)};                                                        \
    }                                                                                                                  \
                                                                                                                       \
    /* Pinhole sides use picture y = cy - fy*y/z, in the affine map's source space. */                                 \
    static inline __attribute__((always_inline)) void P##_frustum(const M##_t* m, const L##_t* lens, R##_t screen,     \
                                                                  P##_t out[PLANE_FRUSTUM_COUNT]) {                    \
        const T zero = OPS##_zero(), one = OPS##_one();                                                                \
        out[PLANE_FRUSTUM_NEAR] = P##_from_rows(m, zero, zero, one, OPS##_neg(lens->near_z));                          \
        out[PLANE_FRUSTUM_LEFT] = P##_from_rows(m, lens->fx, zero, OPS##_sub(lens->cx, screen.x), zero);               \
        out[PLANE_FRUSTUM_RIGHT] =                                                                                     \
            P##_from_rows(m, OPS##_neg(lens->fx), zero, OPS##_sub(R##_right(screen), lens->cx), zero);                 \
        out[PLANE_FRUSTUM_TOP] = P##_from_rows(m, zero, OPS##_neg(lens->fy), OPS##_sub(lens->cy, screen.y), zero);     \
        out[PLANE_FRUSTUM_BOTTOM] = P##_from_rows(m, zero, lens->fy, OPS##_sub(R##_bottom(screen), lens->cy), zero);   \
    }

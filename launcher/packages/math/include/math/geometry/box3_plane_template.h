/* box3_plane_template: how far a real box reaches along a plane: its least and
 * greatest signed distance, and its least objective inside a half-space. */
#pragma once

/* Distance min is plane(centre) - sum |n_k| half_extent_k; max is min of
 * the flipped plane, negated.
 * distance_min_inside is a linear programme solved by duality: the best
 * distance_min(b, objective - lambda constraint) at lambda = 0 and each kink
 * a_k / n_k > 0. Exact when b meets the half-space, a lower bound when not. */
#define MATH_DEFINE_BOX3_PLANE(P, V, L, T, OPS)                                                                        \
    static inline T _##P##_plane_radius(V##_t half_extent, L##_t plane) {                                              \
        return OPS##_add(OPS##_add(OPS##_mul(OPS##_abs(plane.normal.x), half_extent.x),                                \
                                   OPS##_mul(OPS##_abs(plane.normal.y), half_extent.y)),                               \
                         OPS##_mul(OPS##_abs(plane.normal.z), half_extent.z));                                         \
    }                                                                                                                  \
                                                                                                                       \
    static inline T P##_plane_distance_min(P##_t b, L##_t plane) {                                                     \
        return OPS##_sub(L##_distance(plane, P##_centre(b)), _##P##_plane_radius(P##_half_extent(b), plane));          \
    }                                                                                                                  \
                                                                                                                       \
    static inline T P##_plane_distance_max(P##_t b, L##_t plane) {                                                     \
        const L##_t flipped = {V##_scale(plane.normal, OPS##_neg(OPS##_one())), OPS##_neg(plane.d)};                   \
        return OPS##_neg(P##_plane_distance_min(b, flipped));                                                          \
    }                                                                                                                  \
                                                                                                                       \
    static inline T P##_plane_distance_min_inside(P##_t b, L##_t objective, L##_t constraint) {                        \
        const T aim[] = {objective.normal.x, objective.normal.y, objective.normal.z};                                  \
        const T limit[] = {constraint.normal.x, constraint.normal.y, constraint.normal.z};                             \
        T best = P##_plane_distance_min(b, objective);                                                                 \
        for (unsigned k = 0; k < sizeof(aim) / sizeof(aim[0]); k++) {                                                  \
            if (limit[k] == OPS##_zero()) {                                                                            \
                continue;                                                                                              \
            }                                                                                                          \
            const T lambda = OPS##_div(aim[k], limit[k]);                                                              \
            if (!(lambda > OPS##_zero())) {                                                                            \
                continue;                                                                                              \
            }                                                                                                          \
            const L##_t mixed = {V##_sub(objective.normal, V##_scale(constraint.normal, lambda)),                      \
                                 OPS##_sub(objective.d, OPS##_mul(lambda, constraint.d))};                             \
            best = OPS##_max(best, P##_plane_distance_min(b, mixed));                                                  \
        }                                                                                                              \
        return best;                                                                                                   \
    }

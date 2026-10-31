/*
 * box3_template: closed axis-aligned boxes for one number type; see
 * docs/math/Conventions.md#templates for macro arguments. LOWEST and HIGHEST
 * bound the scalar type. Real queries take the plane prefix L and require
 * finite, nonempty boxes.
 */
#pragma once
#include <assert.h>
#include <limits.h>
#include <stdbool.h>

#define MATH_DEFINE_BOX3(P, V, T, OPS, LOWEST, HIGHEST)                                                                \
    typedef struct {                                                                                                   \
        V##_t lo, hi;                                                                                                  \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* empty = (HIGHEST, LOWEST), the identity for extend and union. */                                                \
    static inline P##_t P##_empty(void) { return (P##_t){{HIGHEST, HIGHEST, HIGHEST}, {LOWEST, LOWEST, LOWEST}}; }     \
                                                                                                                       \
    /* empty = any lo_k > hi_k. */                                                                                     \
    static inline bool P##_is_empty(P##_t b) { return b.lo.x > b.hi.x || b.lo.y > b.hi.y || b.lo.z > b.hi.z; }         \
                                                                                                                       \
    /* from_point = (p, p). */                                                                                         \
    static inline P##_t P##_from_point(V##_t p) { return (P##_t){p, p}; }                                              \
                                                                                                                       \
    /* extend = (min(lo, p), max(hi, p)). */                                                                           \
    static inline P##_t P##_extend(P##_t b, V##_t p) { return (P##_t){V##_min(b.lo, p), V##_max(b.hi, p)}; }           \
                                                                                                                       \
    /* union = (min(a.lo, b.lo), max(a.hi, b.hi)). */                                                                  \
    static inline P##_t P##_union(P##_t a, P##_t b) { return (P##_t){V##_min(a.lo, b.lo), V##_max(a.hi, b.hi)}; }      \
                                                                                                                       \
    /* intersection = (max(a.lo, b.lo), min(a.hi, b.hi)); crossed edges are empty. */                                  \
    static inline P##_t P##_intersect(P##_t a, P##_t b) { return (P##_t){V##_max(a.lo, b.lo), V##_min(a.hi, b.hi)}; }  \
                                                                                                                       \
    /* contains_point = all lo_k <= p_k <= hi_k. */                                                                    \
    static inline bool P##_contains_point(P##_t b, V##_t p) {                                                          \
        return p.x >= b.lo.x && p.x <= b.hi.x && p.y >= b.lo.y && p.y <= b.hi.y && p.z >= b.lo.z && p.z <= b.hi.z;     \
    }                                                                                                                  \
                                                                                                                       \
    /* contains = all outer.lo_k <= inner.lo_k and inner.hi_k <= outer.hi_k. */                                        \
    static inline bool P##_contains(P##_t outer, P##_t inner) {                                                        \
        return inner.lo.x >= outer.lo.x && inner.lo.y >= outer.lo.y && inner.lo.z >= outer.lo.z                        \
               && inner.hi.x <= outer.hi.x && inner.hi.y <= outer.hi.y && inner.hi.z <= outer.hi.z;                    \
    }                                                                                                                  \
                                                                                                                       \
    /* overlaps = nonempty intersection, including shared faces. */                                                    \
    static inline bool P##_overlaps(P##_t a, P##_t b) { return !P##_is_empty(P##_intersect(a, b)); }                   \
                                                                                                                       \
    /* closest_point = clamp(p, lo, hi); requires a nonempty box. */                                                   \
    static inline V##_t P##_closest_point(P##_t b, V##_t p) { return V##_clamp(p, b.lo, b.hi); }                       \
                                                                                                                       \
    /* Corner k of eight: bit 0 picks hi.x over lo.x, bit 1 hi.y, bit 2 hi.z. */                                       \
    static inline V##_t P##_corner(P##_t b, int k) {                                                                   \
        return (V##_t){(k & 1) ? b.hi.x : b.lo.x, (k & 2) ? b.hi.y : b.lo.y, (k & 4) ? b.hi.z : b.lo.z};               \
    }

#define MATH_DEFINE_BOX3_REAL(P, V, L, T, OPS)                                                                         \
                                                                                                                       \
    /* centre = 0.5 * (lo + hi). */                                                                                    \
    static inline V##_t P##_centre(P##_t b) { return V##_scale(V##_add(b.lo, b.hi), OPS##_half()); }                   \
                                                                                                                       \
    /* half_extent = 0.5 * (hi - lo). */                                                                               \
    static inline V##_t P##_half_extent(P##_t b) { return V##_scale(V##_sub(b.hi, b.lo), OPS##_half()); }              \
                                                                                                                       \
    /* Distance min is plane(centre) - sum |n_k| half_extent_k; max is min of
     * the flipped plane, negated.
     * distance_min_inside is a linear programme solved by duality: the best
     * distance_min(b, objective - lambda constraint) at lambda = 0 and each kink
     * a_k / n_k > 0. Exact when b meets the half-space, a lower bound when not. */                                          \
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
    }                                                                                                                  \
                                                                                                                       \
    /* Finite endpoints and box values required; slabs give entry in [0,1], with inside starts at t = 0. */            \
    static inline bool P##_segment_hit(P##_t b, V##_t a, V##_t c, T* t) {                                              \
        if (P##_is_empty(b)) {                                                                                         \
            return false;                                                                                              \
        }                                                                                                              \
        T entry = OPS##_zero(), exit = OPS##_one();                                                                    \
        const V##_t delta = V##_sub(c, a);                                                                             \
        const T start[] = {a.x, a.y, a.z}, direction[] = {delta.x, delta.y, delta.z};                                  \
        const T lo[] = {b.lo.x, b.lo.y, b.lo.z}, hi[] = {b.hi.x, b.hi.y, b.hi.z};                                      \
        for (unsigned k = 0; k < sizeof(start) / sizeof(start[0]); k++) {                                              \
            if (direction[k] == OPS##_zero()) {                                                                        \
                if (start[k] < lo[k] || start[k] > hi[k]) {                                                            \
                    return false;                                                                                      \
                }                                                                                                      \
                continue;                                                                                              \
            }                                                                                                          \
            const T first = OPS##_div(OPS##_sub(lo[k], start[k]), direction[k]);                                       \
            const T last = OPS##_div(OPS##_sub(hi[k], start[k]), direction[k]);                                        \
            entry = OPS##_max(entry, OPS##_min(first, last));                                                          \
            exit = OPS##_min(exit, OPS##_max(first, last));                                                            \
            if (entry > exit) {                                                                                        \
                return false;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
        *t = entry;                                                                                                    \
        return true;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    /* d = plane(centre), r = sum |n_k|*extent_k; d+r < 0 rejects, d-r >= 0 clears the bit.  */                        \
    static inline bool P##_outside_planes(P##_t b, const L##_t* planes, int count, unsigned* mask) {                   \
        assert(count >= 0 && (unsigned)count <= sizeof *mask * CHAR_BIT);                                              \
        const V##_t c = P##_centre(b), e = P##_half_extent(b);                                                         \
        for (int p = 0; p < count; p++) {                                                                              \
            if (!(*mask & (1U << p))) {                                                                                \
                continue;                                                                                              \
            }                                                                                                          \
            const L##_t* plane = &planes[p];                                                                           \
            const T d = L##_distance(*plane, c);                                                                       \
            const T r = _##P##_plane_radius(e, *plane);                                                                \
            if (OPS##_add(d, r) < OPS##_zero()) {                                                                      \
                return true;                                                                                           \
            }                                                                                                          \
            if (OPS##_sub(d, r) >= OPS##_zero()) {                                                                     \
                *mask &= ~(1U << p);                                                                                   \
            }                                                                                                          \
        }                                                                                                              \
        return false;                                                                                                  \
    }

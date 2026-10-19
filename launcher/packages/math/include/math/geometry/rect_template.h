/*
 * rect_template: the half-open rectangle for one number type; see
 * docs/math/Conventions.md#templates for the macro arguments. Coordinates and extents use
 * plain arithmetic; callers keep intermediate sums and differences in range.
 */
#pragma once

#include <stdbool.h>

/* The rect from the per-axis picks of a and b: LO for left and top, HI for right and bottom. */
#define MATH_RECT_FOLD(P, OPS, LO, HI, a, b)                                                                           \
    P##_from_edges(OPS##_##LO((a).x, (b).x), OPS##_##LO((a).y, (b).y), OPS##_##HI(P##_right(a), P##_right(b)),         \
                   OPS##_##HI(P##_bottom(a), P##_bottom(b)))

#define MATH_DEFINE_RECT(P, T, OPS)                                                                                    \
    typedef struct {                                                                                                   \
        T x, y, w, h;                                                                                                  \
    } P##_t;                                                                                                           \
                                                                                                                       \
    /* right = x + w, the exclusive right edge. */                                                                     \
    static inline T P##_right(P##_t r) { return r.x + r.w; }                                                           \
                                                                                                                       \
    /* bottom = y + h, the exclusive bottom edge. */                                                                   \
    static inline T P##_bottom(P##_t r) { return r.y + r.h; }                                                          \
                                                                                                                       \
    /* (x, y, w, h) = (left, top, max(right-left, 0), max(bottom-top, 0)), half-open. */                               \
    static inline P##_t P##_from_edges(T left, T top, T right, T bottom) {                                             \
        const P##_t r = {left, top, OPS##_max(right - left, 0), OPS##_max(bottom - top, 0)};                           \
        return r;                                                                                                      \
    }                                                                                                                  \
                                                                                                                       \
    /* empty = (w <= 0 || h <= 0). */                                                                                  \
    static inline bool P##_is_empty(P##_t r) { return r.w <= 0 || r.h <= 0; }                                          \
                                                                                                                       \
    /* intersection = (max(left), max(top), min(right), min(bottom)), clamped to empty. */                             \
    static inline P##_t P##_intersect(P##_t a, P##_t b) { return MATH_RECT_FOLD(P, OPS, max, min, a, b); }             \
                                                                                                                       \
    /* union edges = (min x, min y, max right, max bottom); skip empty; both empty = (a.x,a.y,0,0). */                 \
    static inline P##_t P##_union(P##_t a, P##_t b) {                                                                  \
        if (P##_is_empty(b)) {                                                                                         \
            return P##_is_empty(a) ? P##_from_edges(a.x, a.y, a.x, a.y) : a;                                           \
        }                                                                                                              \
        if (P##_is_empty(a)) {                                                                                         \
            return b;                                                                                                  \
        }                                                                                                              \
        return MATH_RECT_FOLD(P, OPS, min, max, a, b);                                                                 \
    }                                                                                                                  \
                                                                                                                       \
    /* contains = (x <= px < x+w && y <= py < y+h). */                                                                 \
    static inline bool P##_contains(P##_t r, T px, T py) {                                                             \
        return px >= r.x && px < P##_right(r) && py >= r.y && py < P##_bottom(r);                                      \
    }

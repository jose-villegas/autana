/*
 * recti: a half-open integer rectangle, x and y its top-left corner and w by h
 * its size, so it holds the points x <= px < x + w, y <= py < y + h. Edges are
 * computed (recti_right, recti_bottom), never stored. A rect with w or h at or
 * below zero is empty and holds no point; the queries return w and h at zero
 * for an empty result, never negative.
 */
#pragma once

#include <stdbool.h>

#include "math/scalar/mathi.h"

typedef struct {
    int x, y, w, h;
} recti_t;

/* One past the last column inside. */
static inline int
recti_right(recti_t r) {
    return r.x + r.w;
}

/* One past the last row inside. */
static inline int
recti_bottom(recti_t r) {
    return r.y + r.h;
}

/* The rect from its edges, half-open; edges that cross give an empty rect. */
static inline recti_t
recti_from_edges(int left, int top, int right, int bottom) {
    return (recti_t){left, top, mathi_max(right - left, 0), mathi_max(bottom - top, 0)};
}

static inline bool
recti_is_empty(recti_t r) {
    return r.w <= 0 || r.h <= 0;
}

/* The points both hold; empty when they do not overlap. */
static inline recti_t
recti_intersect(recti_t a, recti_t b) {
    return recti_from_edges(mathi_max(a.x, b.x), mathi_max(a.y, b.y), mathi_min(recti_right(a), recti_right(b)),
                            mathi_min(recti_bottom(a), recti_bottom(b)));
}

/* The smallest rect holding both; an empty one adds nothing. */
static inline recti_t
recti_union(recti_t a, recti_t b) {
    if (recti_is_empty(a)) {
        return b;
    }
    if (recti_is_empty(b)) {
        return a;
    }
    return recti_from_edges(mathi_min(a.x, b.x), mathi_min(a.y, b.y), mathi_max(recti_right(a), recti_right(b)),
                            mathi_max(recti_bottom(a), recti_bottom(b)));
}

static inline bool
recti_contains(recti_t r, int px, int py) {
    return px >= r.x && px < recti_right(r) && py >= r.y && py < recti_bottom(r);
}

#include "math/geometry/rect_overlap.h"

#include <stdlib.h>

static int
by_left(const void* a, const void* b) {
    return ((const recti_t*)a)->x - ((const recti_t*)b)->x;
}

static int
by_top(const void* a, const void* b) {
    return ((const recti_t*)a)->y - ((const recti_t*)b)->y;
}

/* Whether `b`, sorted after `a`, starts before `a` ends along the axis. */
static bool
starts_within(recti_t a, recti_t b, bool across) {
    return across ? b.x < recti_right(a) : b.y < recti_bottom(a);
}

int
recti_overlapping_pair(recti_t* r, int count, bool across, int* j) {
    qsort(r, (size_t)count, sizeof r[0], across ? by_left : by_top);
    for (int i = 0; i < count; i++) {
        for (*j = i + 1; *j < count && starts_within(r[i], r[*j], across); (*j)++) {
            if (!recti_is_empty(recti_intersect(r[i], r[*j]))) {
                return i;
            }
        }
    }
    return -1;
}

int
recti_subtract(recti_t a, recti_t b, recti_t out[RECTI_SUBTRACT_MAX]) {
    const recti_t cut = recti_intersect(a, b);
    if (recti_is_empty(cut)) {
        out[0] = a;
        return recti_is_empty(a) ? 0 : 1;
    }
    const recti_t pieces[RECTI_SUBTRACT_MAX] = {
        recti_from_edges(a.x, a.y, recti_right(a), cut.y),
        recti_from_edges(a.x, recti_bottom(cut), recti_right(a), recti_bottom(a)),
        recti_from_edges(a.x, cut.y, cut.x, recti_bottom(cut)),
        recti_from_edges(recti_right(cut), cut.y, recti_right(a), recti_bottom(cut)),
    };
    int n = 0;
    for (int i = 0; i < RECTI_SUBTRACT_MAX; i++) {
        if (!recti_is_empty(pieces[i])) {
            out[n++] = pieces[i];
        }
    }
    return n;
}

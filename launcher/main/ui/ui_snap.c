#include "ui/ui_snap.h"

static int
nearest_edge(int p, int start, int length) {
    const int end = start + length;
    if (p < start) {
        return start;
    }
    if (p >= end) {
        return end - 1;
    }
    return p;
}

mu_Vec2
ui_snap_point(const mu_Rect* rects, int count, mu_Vec2 point, int reach) {
    if (reach <= 0 || rects == NULL) {
        return point;
    }
    if (count > UI_SNAP_RECTS_MAX) {
        count = UI_SNAP_RECTS_MAX;
    }

    int best_distance2 = reach * reach + 1;
    mu_Vec2 best = point;
    for (int i = 0; i < count; i++) {
        const mu_Vec2 candidate = {
            .x = nearest_edge(point.x, rects[i].x, rects[i].w),
            .y = nearest_edge(point.y, rects[i].y, rects[i].h),
        };
        const int dx = candidate.x - point.x;
        const int dy = candidate.y - point.y;
        const int distance2 = dx * dx + dy * dy;
        if (distance2 < best_distance2) {
            best_distance2 = distance2;
            best = candidate;
        }
    }
    return best;
}

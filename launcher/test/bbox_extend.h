/*
 * bbox_extend - one bounding-box widening step, shared by every suite that
 * tracks the inclusive min/max span of cells or pixels it has touched.
 *
 * Header-only (static inline) so a suite anywhere under launcher/main/ or
 * launcher/test/ can pull it in without a new compile-list entry in either
 * run_tests.sh or the device test build.
 */
#pragma once

/* Widens [*min_x,*max_x] x [*min_y,*max_y] to include (x, y) - both ends
 * inclusive, so an empty box starts at (max < min) and a single point
 * leaves min == max on that axis. */
static inline void
bbox_extend_inclusive(int x, int y, int* min_x, int* max_x, int* min_y, int* max_y) {
    if (x < *min_x) {
        *min_x = x;
    }
    if (x > *max_x) {
        *max_x = x;
    }
    if (y < *min_y) {
        *min_y = y;
    }
    if (y > *max_y) {
        *max_y = y;
    }
}

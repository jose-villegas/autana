/*
 * rt_refine - the order a progressive tracer visits pixels in: coarse to
 * fine on halving lattices, 8, 4, 2, then every pixel. A pass traces only the
 * lattice points no coarser pass reached and paints each as a step x step
 * block, so a whole blocky picture exists after 1/64 of the work and no pixel
 * is ever traced twice.
 *
 * Each pass still sweeps top to bottom. A scattered order over the whole
 * screen would resolve the same picture but dirty every row on every frame,
 * and the panel transfer is the cost that matters here.
 */
#pragma once

#include <stdbool.h>

#define RT_REFINE_FIRST_STEP 8
#define RT_REFINE_PASSES     4

/* (x, y) must already lie on the `step` lattice. */
static inline bool
rt_refine_is_new(int x, int y, int step) {
    if (step == RT_REFINE_FIRST_STEP) {
        return true;
    }
    const int coarser = step * 2;
    return (x % coarser) != 0 || (y % coarser) != 0;
}

static inline int
rt_refine_next_step(int step) {
    return step / 2; /* 0 once the every-pixel pass is done */
}

/* 1-based, for a progress readout. */
static inline int
rt_refine_pass_number(int step) {
    int pass = 1;
    for (int s = RT_REFINE_FIRST_STEP; s > step; s /= 2) {
        pass++;
    }
    return pass;
}

/* How many pixels tracing row `y` at `step` draws over a row `width` wide -
 * what rt_refine_is_new() sums to for one row, priced before any pixel in
 * it is actually traced. */
static inline int
rt_refine_row_cost(int width, int y, int step) {
    int cost = 0;
    for (int x = 0; x < width; x += step) {
        if (rt_refine_is_new(x, y, step)) {
            cost++;
        }
    }
    return cost;
}

/* The row just past a `pixel_budget`-limited lattice range from `y0`: whole
 * rows on `step`'s stride, priced by rt_refine_row_cost(), until their sum
 * reaches `pixel_budget` or `height`. Pure - a caller splits the range
 * after this returns, never during. */
static inline int
rt_refine_lattice_range_end(int width, int height, int y0, int step, int pixel_budget) {
    int y = y0;
    int traced = 0;

    while (y < height && traced < pixel_budget) {
        traced += rt_refine_row_cost(width, y, step);
        y += step;
    }
    return y;
}

/* The same range-end rule for a pass whose every row costs the same
 * `row_cost` pixels - a full-width sweep, not a lattice. */
static inline int
rt_refine_uniform_range_end(int height, int y0, int row_cost, int pixel_budget) {
    int y = y0;
    int traced = 0;

    while (y < height && traced < pixel_budget) {
        traced += row_cost;
        y++;
    }
    return y;
}

/* The row [y0, y1) - on `step`'s stride - splits at, so two independent
 * traces of [y0, mid) and [mid, y1) cover the same rows as one trace of the
 * whole range, each exactly once. Degenerate at zero or one row: the first
 * half comes back empty rather than off-lattice. */
static inline int
rt_refine_split_mid(int y0, int y1, int step) {
    const int rows = (y1 - y0) / step;
    return y0 + (rows / 2) * step;
}

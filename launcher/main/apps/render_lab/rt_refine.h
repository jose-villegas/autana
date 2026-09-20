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

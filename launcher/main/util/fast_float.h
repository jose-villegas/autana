/*
 * fast_float: a ceiling that stays inline. ceilf() is a call into libm,
 * which in a per-row loop costs more than the work around it. Division needs
 * no such help: libgcc's __divsf3 on this chip is the FPU's own div0.s and
 * divn.s sequence, and a hand-rolled reciprocal measured slower.
 */
#pragma once

/* For |x| below 2^31. */
static inline int
fast_ceil(float x) {
    const int i = (int)x;
    return i + ((float)i < x);
}

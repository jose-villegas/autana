/*
 * touch_calib - undoes a touch panel's measured misreporting. A panel that
 * reports a tap at `reported = A * true + offset`, per axis, is corrected by
 * the inverse of that map: the map is fitted offline from logged taps and
 * compiled in, and the inverse applied to every controller point before
 * anything reads it.
 *
 * Affine and no more: a finger's own scatter, about 1.2 mm, is larger than
 * anything a higher-order fit of the panel could still recover.
 */
#pragma once

#include <stdint.h>

/* How the panel reports a tap aimed at (x, y), as a fit prints it:
 *     reported_x = xx*x + xy*y + x0        reported_y = yx*x + yy*y + y0 */
typedef struct {
    float xx, xy, x0;
    float yx, yy, y0;
} touch_calib_fit_t;

/* The inverse, in 16.16 fixed point so a correction is integer arithmetic. */
typedef struct {
    int32_t xx, xy, x0;
    int32_t yx, yy, y0;
} touch_calib_t;

touch_calib_t touch_calib_from_fit(touch_calib_fit_t fit);

/* Corrects one raw point in place, kept inside a `w` x `h` panel. */
void touch_calib_apply(const touch_calib_t* calib, int w, int h, int* x, int* y);

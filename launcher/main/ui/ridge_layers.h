/*
 * ridge_layers - portable bounds for repainting a dithered ridge edge.
 */
#pragma once

#include <stdint.h>

/* The rows which may differ when a curve moves from `was_q4` to `now_q4`.
 * Both the dithered lip below the curve and the caller's light reach are
 * included; the returned interval is half open and in whole pixels. */
static inline void
ridge_layer_dirty_span(int was_q4, int now_q4, int lip_px, int reach_px, int* lo, int* hi) {
    const int low_q4 = was_q4 < now_q4 ? was_q4 : now_q4;
    const int high_q4 = was_q4 > now_q4 ? was_q4 : now_q4;
    *lo = (low_q4 >> 4) - reach_px;
    *hi = ((high_q4 + 15) >> 4) + lip_px + reach_px;
}

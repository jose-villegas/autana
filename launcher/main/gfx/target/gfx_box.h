/* gfx_box: pixel segment clipping. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/geometry/recti.h"

/* Cohen-Sutherland outcodes: one bit per edge the point lies outside of. */
enum { GFX_BOX_OUT_LEFT = 1, GFX_BOX_OUT_RIGHT = 2, GFX_BOX_OUT_TOP = 4, GFX_BOX_OUT_BOTTOM = 8 };

static inline int
gfx_box_outcode(const recti_t* clip, int x, int y) {
    int code = 0;
    if (x < clip->x) {
        code |= GFX_BOX_OUT_LEFT;
    } else if (x >= recti_right(*clip)) {
        code |= GFX_BOX_OUT_RIGHT;
    }
    if (y < clip->y) {
        code |= GFX_BOX_OUT_TOP;
    } else if (y >= recti_bottom(*clip)) {
        code |= GFX_BOX_OUT_BOTTOM;
    }
    return code;
}

/* Clipping to the last pixel inside preserves the rasterizer's endpoint rounding. */
static inline bool
gfx_box_clip_segment(const recti_t* clip, int* x0, int* y0, int* x1, int* y1) {
    int c0 = gfx_box_outcode(clip, *x0, *y0);
    int c1 = gfx_box_outcode(clip, *x1, *y1);

    for (int pass = 0; pass < 8; pass++) {
        if ((c0 | c1) == 0) {
            return true; /* both ends inside */
        }
        if ((c0 & c1) != 0) {
            return false; /* both beyond the same edge */
        }

        const int out = c0 ? c0 : c1;
        int x, y;

        /* Clips to last pixel inside, not boundary. */
        if (out & GFX_BOX_OUT_BOTTOM) {
            y = recti_bottom(*clip) - 1;
            x = *x0 + (int)(((int64_t)(*x1 - *x0) * (y - *y0)) / (*y1 - *y0));
        } else if (out & GFX_BOX_OUT_TOP) {
            y = clip->y;
            x = *x0 + (int)(((int64_t)(*x1 - *x0) * (y - *y0)) / (*y1 - *y0));
        } else if (out & GFX_BOX_OUT_RIGHT) {
            x = recti_right(*clip) - 1;
            y = *y0 + (int)(((int64_t)(*y1 - *y0) * (x - *x0)) / (*x1 - *x0));
        } else {
            x = clip->x;
            y = *y0 + (int)(((int64_t)(*y1 - *y0) * (x - *x0)) / (*x1 - *x0));
        }

        if (out == c0) {
            *x0 = x;
            *y0 = y;
            c0 = gfx_box_outcode(clip, x, y);
        } else {
            *x1 = x;
            *y1 = y;
            c1 = gfx_box_outcode(clip, x, y);
        }
    }
    return false;
}

/* gfx_box: half-open pixel rectangle. */
#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#include "util/scalar/intmath.h"

typedef struct {
    int x0, y0, x1, y1;
} gfx_box_t;

#define GFX_BOX_EMPTY ((gfx_box_t){INT_MAX, INT_MAX, INT_MIN, INT_MIN})

static inline bool
gfx_box_is_empty(gfx_box_t box) {
    return box.x0 >= box.x1 || box.y0 >= box.y1;
}

static inline void
gfx_box_extend(gfx_box_t* box, gfx_box_t addition) {
    if (gfx_box_is_empty(addition)) {
        return;
    }
    box->x0 = im_min(box->x0, addition.x0);
    box->y0 = im_min(box->y0, addition.y0);
    box->x1 = im_max(box->x1, addition.x1);
    box->y1 = im_max(box->y1, addition.y1);
}

/* Cohen-Sutherland outcodes: one bit per edge the point lies outside of. */
enum { GFX_BOX_OUT_LEFT = 1, GFX_BOX_OUT_RIGHT = 2, GFX_BOX_OUT_TOP = 4, GFX_BOX_OUT_BOTTOM = 8 };

static inline int
gfx_box_outcode(const gfx_box_t* clip, int x, int y) {
    int code = 0;
    if (x < clip->x0) {
        code |= GFX_BOX_OUT_LEFT;
    } else if (x >= clip->x1) {
        code |= GFX_BOX_OUT_RIGHT;
    }
    if (y < clip->y0) {
        code |= GFX_BOX_OUT_TOP;
    } else if (y >= clip->y1) {
        code |= GFX_BOX_OUT_BOTTOM;
    }
    return code;
}

/* Clipping to the last pixel inside preserves the rasterizer's endpoint rounding. */
static inline bool
gfx_box_clip_segment(const gfx_box_t* clip, int* x0, int* y0, int* x1, int* y1) {
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
            y = clip->y1 - 1;
            x = *x0 + (int)(((int64_t)(*x1 - *x0) * (y - *y0)) / (*y1 - *y0));
        } else if (out & GFX_BOX_OUT_TOP) {
            y = clip->y0;
            x = *x0 + (int)(((int64_t)(*x1 - *x0) * (y - *y0)) / (*y1 - *y0));
        } else if (out & GFX_BOX_OUT_RIGHT) {
            x = clip->x1 - 1;
            y = *y0 + (int)(((int64_t)(*y1 - *y0) * (x - *x0)) / (*x1 - *x0));
        } else {
            x = clip->x0;
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

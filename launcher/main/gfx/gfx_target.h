/*
 * gfx_target - the buffer every pixel-writing gfx_* primitive actually
 * writes into: the whole framebuffer in GFX_LAYOUT_FULL_FB, or one band's
 * own buffer while a band is being rendered in GFX_LAYOUT_BANDS
 * (gfx_mode.h). A standalone, ESP-IDF-free module, the same reason
 * gfx_dirty.h is, so a host suite can drive the real clip-and-translate
 * arithmetic without a panel or a framebuffer.
 *
 * Every primitive already clips to the app's own clip rect (gfx_set_clip());
 * a target additionally clips to its own row range and, for a band buffer,
 * shorter than the whole screen, translates an absolute row into that
 * buffer's own local row 0. Full-fb mode's target (y0 0, height GFX_HEIGHT)
 * makes both of those a no-op, which is what keeps it byte-identical.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gfx/gfx_color.h"

typedef struct {
    gfx_color_t* buf; /* row 0 of this buffer is absolute row y0 */
    int y0;
    int height;
    int stride; /* pixels per row */
} gfx_target_t;

/* The absolute row range [y0, y1) intersected with `clip`'s and the
 * target's own row range - never wider than either. Callers that already
 * have an x range apply their own clip.x0/x1 separately; only y depends on
 * the target. */
static inline void
gfx_target_clip_y(gfx_target_t target, int clip_y0, int clip_y1, int* y0, int* y1) {
    if (*y0 < clip_y0) {
        *y0 = clip_y0;
    }
    if (*y1 > clip_y1) {
        *y1 = clip_y1;
    }
    if (*y0 < target.y0) {
        *y0 = target.y0;
    }
    if (*y1 > target.y0 + target.height) {
        *y1 = target.y0 + target.height;
    }
}

/* The row pointer for absolute row `y`, once it is known to lie inside
 * [target.y0, target.y0 + target.height) - gfx_target_clip_y() above is
 * what a caller uses to know that. */
static inline gfx_color_t*
gfx_target_row(gfx_target_t target, int y) {
    return target.buf + (size_t)(y - target.y0) * target.stride;
}

/* True if a shape whose own row extent is [y0, y1) has anything at all to
 * draw into this target - the check a caller decides whether to bother
 * drawing with, before paying for the call: a per-band triangle bin and
 * ui.c's per-band command replay both use this to skip work outside the
 * current band rather than discover it clips to nothing. */
static inline bool
gfx_target_row_range_overlaps(gfx_target_t target, int y0, int y1) {
    return y0 < target.y0 + target.height && y1 > target.y0;
}

/* gfx_fill_rect()'s own body, extracted here so the exact arithmetic every
 * fill goes through is the one a host suite can drive directly. Absolute
 * (x, y, w, h) and `clip` are in full-screen space, the same as every
 * gfx_fill_rect() caller already uses; only the write lands in the
 * target's own local rows. Reports the actually-painted rect back through
 * the out params - gfx.c's own caller needs it (already clipped) to mark
 * the right dirty region. */
static inline void
gfx_target_fill_rect(gfx_target_t target, int clip_x0, int clip_y0, int clip_x1, int clip_y1, int x, int y, int w,
                     int h, gfx_color_t color, int* out_x0, int* out_y0, int* out_x1, int* out_y1) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;

    if (x0 < clip_x0) {
        x0 = clip_x0;
    }
    if (x1 > clip_x1) {
        x1 = clip_x1;
    }
    gfx_target_clip_y(target, clip_y0, clip_y1, &y0, &y1);

    for (int row = y0; row < y1; row++) {
        gfx_color_t* dst = gfx_target_row(target, row) + x0;
        for (int col = x0; col < x1; col++) {
            *dst++ = color;
        }
    }

    *out_x0 = x0;
    *out_y0 = y0;
    *out_x1 = x1;
    *out_y1 = y1;
}

/*
 * gfx_target: the framebuffer, a band buffer or a replay strip, with absolute row bounds.
 * Primitives intersect present's clip with the target and translate to local
 * rows. For full framebuffers, clip-to-rows and translation are no-ops.
 * Pure arithmetic lets host suites verify both without a panel.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gfx/target/gfx_color.h"
#include "math/geometry/recti.h"

typedef struct {
    gfx_color_t* buf; /* row 0 of this buffer is absolute row y0 */
    int y0;
    int height;
    int stride; /* pixels per row */
} gfx_target_t;

static inline recti_t
gfx_target_clip_rect(gfx_target_t target, recti_t clip, recti_t rect) {
    return recti_from_edges(mathi_max(rect.x, clip.x), mathi_max(mathi_max(rect.y, clip.y), target.y0),
                            mathi_min(recti_right(rect), recti_right(clip)),
                            mathi_min(mathi_min(recti_bottom(rect), recti_bottom(clip)), target.y0 + target.height));
}

/* The row pointer for absolute row `y`, once it is known to lie inside
 * [target.y0, target.y0 + target.height): gfx_target_clip_rect() is
 * what a caller uses to know that. */
static inline gfx_color_t*
gfx_target_row(gfx_target_t target, int y) {
    return target.buf + (size_t)(y - target.y0) * target.stride;
}

/* True if a shape whose own row extent is [y0, y1) has anything at all to
 * draw into this target, the check a caller decides whether to bother
 * drawing with, before paying for the call: a per-band triangle bin and
 * ui.c's per-band command replay both use this to skip work outside the
 * current band rather than discover it clips to nothing. */
static inline bool
gfx_target_row_range_overlaps(gfx_target_t target, int y0, int y1) {
    return y0 < target.y0 + target.height && y1 > target.y0;
}

/* Fills a screen-space shape and returns its clipped dirty rectangle.
 * Writes land in the target's local rows. */
static inline recti_t
gfx_target_fill_rect(gfx_target_t target, recti_t clip, recti_t shape, gfx_color_t color) {
    const recti_t rect = gfx_target_clip_rect(target, clip, shape);
    for (int row = rect.y; row < recti_bottom(rect); row++) {
        gfx_color_t* dst = gfx_target_row(target, row) + rect.x;
        for (int i = 0; i < rect.w; i++) {
            *dst++ = color;
        }
    }
    return rect;
}

/* A source pixel as shown: `clear` where nothing was drawn. */
static inline uint16_t
gfx_target_shown_pixel(const uint16_t* input, const uint16_t* input_depth, int x, uint16_t clear) {
    return input_depth != NULL && input_depth[x] == 0 ? clear : input[x];
}

/* Paired 32-bit stores share a source row across repeated output rows.
 * NULL rows selects exact 2x expansion; target.y0 addresses a strip.
 * A nonzero dark_run shades its latter half, with the supplied keep fraction in GFX_COLOR_SCALE_UNIT. */
static inline __attribute__((always_inline)) void
gfx_target_paired_rows_shaded(gfx_target_t target, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                              int source_width, const uint16_t* rows, int first_row, int row_count, bool doubled,
                              int dark_run, unsigned keep) {
    const int end = first_row + row_count;
    const int stores = target.stride / 2;
    for (int y = first_row; y < end;) {
        const int source_y = rows == NULL ? y / 2 : rows[y];
        const size_t row = (size_t)source_y * source_width;
        const uint16_t* input = source + row;
        const uint16_t* input_depth = depth == NULL ? NULL : depth + row;
        uint32_t* top = (uint32_t*)gfx_target_row(target, y);
        uint32_t* bottom = top + stores;
        const bool both = y + 1 < end && (rows == NULL ? (y + 1) / 2 : rows[y + 1]) == source_y;
        const bool top_dark = dark_run != 0 && y % dark_run >= dark_run / 2;
        const bool bottom_dark = dark_run != 0 && (y + 1) % dark_run >= dark_run / 2;
        const bool needs_dark = top_dark || (both && bottom_dark);
        for (int x = 0; x < stores; x++) {
            const uint32_t pair =
                doubled ? 0x10001U * gfx_target_shown_pixel(input, input_depth, x, clear)
                        : gfx_target_shown_pixel(input, input_depth, 2 * x, clear)
                              | ((uint32_t)gfx_target_shown_pixel(input, input_depth, (2 * x) + 1, clear) << 16);
            const uint32_t dark = needs_dark ? gfx_color_pair_scale(pair, keep) : 0;
            top[x] = top_dark ? dark : pair;
            if (both) {
                bottom[x] = bottom_dark ? dark : pair;
            }
        }
        y += both ? 2 : 1;
    }
}

static inline __attribute__((always_inline)) void
gfx_target_paired_rows(gfx_target_t target, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                       int source_width, const uint16_t* rows, int first_row, int row_count, bool doubled) {
    gfx_target_paired_rows_shaded(target, source, depth, clear, source_width, rows, first_row, row_count, doubled, 0,
                                  GFX_COLOR_SCALE_UNIT);
}

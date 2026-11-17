/*
 * gfx_target: a picture as a set of image attachments sharing picture rows: the
 * framebuffer, a band buffer or a replay strip (colour alone), or a renderer's
 * colour, depth and further per-pixel maps. Every attachment has the target's
 * width and rows, so one row index finds a pixel in all of them; a target may be
 * a window of a taller picture, each attachment then starting at first_row.
 * Primitives intersect present's clip with the target and translate to local
 * rows; for full framebuffers both are no-ops. Pure arithmetic lets host suites
 * verify it without a panel; what an attachment means beyond its format is its
 * renderer's.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gfx/target/gfx_color.h"
#include "image/image.h"
#include "simd/copy.h"
#include "simd/fill.h"
#include "simd/repeat.h"
#include "simd/simd.h"

#define GFX_PIXEL_FORMAT            IMAGE_RGB565BE
#define GFX_ATTACHMENT_COLOR        0
#define GFX_ATTACHMENT_DEPTH        1
#define GFX_ATTACHMENT_FURTHER      2
#define GFX_ATTACHMENTS_MAX         4

/* The attachments a draw writes beside colour and depth, at most. */
#define GFX_ATTACHMENTS_FURTHER_MAX (GFX_ATTACHMENTS_MAX - GFX_ATTACHMENT_FURTHER)

typedef struct {
    int first_row; /* the picture row each attachment's row 0 stands for */
    int count;
    image_t attachment[];
} gfx_target_t;

/* The union exposes fixed storage through the flexible target view. */
#define GFX_TARGET_STORAGE(n)                                                                                          \
    union {                                                                                                            \
        struct {                                                                                                       \
            int first_row;                                                                                             \
            int count;                                                                                                 \
            image_t attachment[n];                                                                                     \
        };                                                                                                             \
        gfx_target_t target;                                                                                           \
    }

#define GFX_TARGET_COLOR_INIT(pixels_, width_, height_)                                                                \
    {                                                                                                                  \
        .first_row = 0, .count = 1, .attachment = {                                                                    \
            {pixels_, GFX_PIXEL_FORMAT, width_, height_, IMAGE_PITCH(GFX_PIXEL_FORMAT, width_)}                        \
        }                                                                                                              \
    }

static inline void
gfx_target_of_color(gfx_target_t* out, void* pixels, int width, int height, int first_row) {
    out->first_row = first_row;
    out->count = 1;
    out->attachment[0] = (image_t){pixels, GFX_PIXEL_FORMAT, width, height, IMAGE_PITCH(GFX_PIXEL_FORMAT, width)};
}

/* Colour and depth alone, `height` rows from picture row `first_row`. */
static inline void
gfx_target_of_color_depth(gfx_target_t* out, void* color, uint16_t* depth, int width, int height, int first_row) {
    gfx_target_of_color(out, color, width, height, first_row);
    out->count = GFX_ATTACHMENT_FURTHER;
    out->attachment[GFX_ATTACHMENT_DEPTH] = (image_t){depth, IMAGE_U16, width, height, IMAGE_PITCH(IMAGE_U16, width)};
}

static inline int
gfx_target_end_row(const gfx_target_t* target) {
    return target->first_row + target->attachment[0].height;
}

/* Rounded to a word so each carved attachment starts aligned. */
static inline size_t
gfx_attachment_bytes(image_format_t format, int width, int rows) {
    const size_t pitch = IMAGE_PITCH(format, width);
    return (pitch * (size_t)rows + 3U) & ~(size_t)3U;
}

static inline size_t
gfx_target_bytes(const gfx_target_t* target) {
    size_t bytes = 0;
    for (int i = 0; i < target->count; i++) {
        bytes += gfx_attachment_bytes(target->attachment[i].format, target->attachment[0].width,
                                      target->attachment[0].height);
    }
    return bytes;
}

static inline char*
gfx_target_carve(gfx_target_t* target, void* block) {
    char* p = block;
    const int width = target->attachment[0].width;
    const int height = target->attachment[0].height;
    for (int i = 0; i < target->count; i++) {
        image_t* a = &target->attachment[i];
        a->pixels = p;
        a->width = width;
        a->height = height;
        a->pitch = IMAGE_PITCH(a->format, width);
        assert(i != GFX_ATTACHMENT_DEPTH || a->pitch == target->attachment[GFX_ATTACHMENT_COLOR].pitch);
        p += gfx_attachment_bytes(a->format, width, height);
    }
    return p;
}

static inline void
gfx_target_window(gfx_target_t* window, const gfx_target_t* target, int row0, int row1) {
    assert(row0 >= target->first_row && row1 >= row0 && row1 <= gfx_target_end_row(target));
    window->first_row = row0;
    window->count = target->count;
    for (int i = 0; i < target->count; i++) {
        const image_t* a = &target->attachment[i];
        assert(a->width == target->attachment[0].width && a->height == target->attachment[0].height);
        window->attachment[i] = *a;
        window->attachment[i].pixels = image_row(a, row0 - target->first_row);
        window->attachment[i].height = row1 - row0;
    }
}

/* The attachment's row pointer for picture row `y`, once it is known to lie
 * inside [first_row, gfx_target_end_row()): gfx_target_clip_rect() is what a
 * caller uses to know that. */
static inline void*
gfx_target_row(const gfx_target_t* target, int index, int y) {
    return image_row(&target->attachment[index], y - target->first_row);
}

/* The row pointer for picture row `y`, once it is known to lie inside
 * [first_row, gfx_target_end_row()): gfx_target_clip_rect() is what a caller
 * uses to know that. */
static inline gfx_color_t*
gfx_target_color(const gfx_target_t* target, int y) {
    return gfx_target_row(target, GFX_ATTACHMENT_COLOR, y);
}

static inline uint16_t*
gfx_target_depth(const gfx_target_t* target, int y) {
    return gfx_target_row(target, GFX_ATTACHMENT_DEPTH, y);
}

static inline recti_t
gfx_target_clip_rect(const gfx_target_t* target, recti_t clip, recti_t rect) {
    const int first = target->first_row;
    return recti_from_edges(mathi_max(rect.x, clip.x), mathi_max(mathi_max(rect.y, clip.y), first),
                            mathi_min(recti_right(rect), recti_right(clip)),
                            mathi_min(mathi_min(recti_bottom(rect), recti_bottom(clip)), gfx_target_end_row(target)));
}

/* Every pixel of the target set to `colour`: a target's rows are whole, so
 * the buffer is one run. */
static inline void
gfx_target_fill(const gfx_target_t* target, gfx_color_t colour) {
    simd_fill_u16(target->attachment[0].pixels, colour,
                  ((size_t)target->attachment[0].pitch * target->attachment[0].height) / sizeof(uint16_t));
}

/* True if a shape whose own row extent is [y0, y1) has anything at all to
 * draw into this target, the check a caller decides whether to bother
 * drawing with, before paying for the call: a per-band triangle bin and
 * ui.c's per-band command replay both use this to skip work outside the
 * current band rather than discover it clips to nothing. */
static inline bool
gfx_target_row_range_overlaps(const gfx_target_t* target, int y0, int y1) {
    return y0 < gfx_target_end_row(target) && y1 > target->first_row;
}

/* True if the target holds every row of [y0, y1), so a shape drawn there
 * needs no per-row check. */
static inline bool
gfx_target_holds_rows(const gfx_target_t* target, int y0, int y1) {
    return y0 >= target->first_row && y1 <= gfx_target_end_row(target);
}

/* Fills a screen-space shape and returns its clipped dirty rectangle.
 * Writes land in the target's local rows. */
static inline recti_t
gfx_target_fill_rect(const gfx_target_t* target, recti_t clip, recti_t shape, gfx_color_t color) {
    const recti_t rect = gfx_target_clip_rect(target, clip, shape);
    for (int row = rect.y; row < recti_bottom(rect); row++) {
        simd_fill_u16(gfx_target_color(target, row) + rect.x, color, (size_t)rect.w);
    }
    return rect;
}

/* A seam mask: one bit per pixel, set where a filter must not blend the pixel
 * with the one right of it or below it. Bit x % GFX_SEAM_BITS of byte
 * x / GFX_SEAM_BITS of its row. */
#define GFX_SEAM_BITS                 8
#define GFX_SEAM_PITCH(width)         (((width) + GFX_SEAM_BITS - 1) / GFX_SEAM_BITS)
#define GFX_SEAM_BYTES(width, height) ((size_t)GFX_SEAM_PITCH(width) * (size_t)(height))

static inline bool
gfx_seam_at(const uint8_t* seams, int x) {
    return (seams[x / GFX_SEAM_BITS] >> (x % GFX_SEAM_BITS)) & 1U;
}

/* A source pixel as shown: `clear` where nothing was drawn. */
static inline uint16_t
gfx_target_shown_pixel(const uint16_t* input, const uint16_t* input_depth, int x, uint16_t clear) {
    return input_depth != NULL && input_depth[x] == 0 ? clear : input[x];
}

/* Paired 32-bit stores share a source row across repeated output rows.
 * NULL rows selects exact 2x expansion; target->first_row addresses a strip.
 * A nonzero dark_run shades its latter half, with the supplied keep fraction in RGB565_SCALE_UNIT. */
static inline __attribute__((always_inline)) void
gfx_target_paired_rows_shaded(const gfx_target_t* target, const image_t* source, const uint16_t* depth, uint16_t clear,
                              const uint16_t* rows, int first_row, int row_count, bool doubled, int dark_run,
                              unsigned keep) {
    const uint16_t* color = source->pixels;
    const size_t source_pitch = source->pitch;
    const size_t target_pitch = target->attachment[GFX_ATTACHMENT_COLOR].pitch;
    char* output = target->attachment[GFX_ATTACHMENT_COLOR].pixels;
    const int row0 = target->first_row;
    const int end = first_row + row_count;
    const int stores = target_pitch / sizeof(uint32_t);
    /* Nothing per pixel but the doubling: a row is one repeat, and a second
     * row from the same source row is a copy of the first. */
    const bool repeat_only = doubled && depth == NULL && dark_run == 0;
    for (int y = first_row; y < end;) {
        const int source_y = rows == NULL ? y / 2 : rows[y];
        const uint16_t* input = (const uint16_t*)((const char*)color + (size_t)source_y * source_pitch);
        const uint16_t* input_depth =
            depth == NULL ? NULL : (const uint16_t*)((const char*)depth + (size_t)source_y * source_pitch);
        uint32_t* top = (uint32_t*)(output + (size_t)(y - row0) * target_pitch);
        uint32_t* bottom = top + stores;
        const bool both = y + 1 < end && (rows == NULL ? (y + 1) / 2 : rows[y + 1]) == source_y;
        const bool top_dark = dark_run != 0 && y % dark_run >= dark_run / 2;
        const bool bottom_dark = dark_run != 0 && (y + 1) % dark_run >= dark_run / 2;
        const bool needs_dark = top_dark || (both && bottom_dark);
        if (repeat_only) {
            simd_repeat2_u16((uint16_t*)top, input, (size_t)stores);
            if (both) {
                simd_copy(bottom, top, target_pitch);
            }
        }
        for (int x = 0; x < stores && !repeat_only; x++) {
            const uint32_t pair =
                doubled ? SIMD_U16_PAIR(gfx_target_shown_pixel(input, input_depth, x, clear))
                        : gfx_target_shown_pixel(input, input_depth, 2 * x, clear)
                              | ((uint32_t)gfx_target_shown_pixel(input, input_depth, (2 * x) + 1, clear) << 16);
            const uint32_t dark = needs_dark ? rgb565be_pair_scale(pair, keep) : 0;
            top[x] = top_dark ? dark : pair;
            if (both) {
                bottom[x] = bottom_dark ? dark : pair;
            }
        }
        y += both ? 2 : 1;
    }
}

static inline __attribute__((always_inline)) void
gfx_target_paired_rows(const gfx_target_t* target, const gfx_target_t* source, uint16_t clear, const uint16_t* rows,
                       int first_row, int row_count, bool doubled) {
    const uint16_t* depth =
        source->count > GFX_ATTACHMENT_DEPTH ? source->attachment[GFX_ATTACHMENT_DEPTH].pixels : NULL;
    gfx_target_paired_rows_shaded(target, &source->attachment[GFX_ATTACHMENT_COLOR], depth, clear, rows, first_row,
                                  row_count, doubled, 0, RGB565_SCALE_UNIT);
}

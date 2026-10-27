/* gfx_target: image attachments sharing picture coordinates, for frames, bands and render maps. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "gfx/target/gfx_color.h"
#include "image/image.h"

#define GFX_PIXEL_FORMAT       IMAGE_RGB565BE
#define GFX_ATTACHMENT_COLOR   0
#define GFX_ATTACHMENT_DEPTH   1
#define GFX_ATTACHMENT_FURTHER 2
#define GFX_ATTACHMENTS_MAX    4

typedef struct {
    int first_row; /* the picture row each attachment's row 0 stands for */
    int count;
    image_t attachment[];
} gfx_target_t;

/* Pass the storage's address as gfx_target_t*, or use a union. */
#define GFX_TARGET_STORAGE(n)                                                                                          \
    struct {                                                                                                           \
        int first_row;                                                                                                 \
        int count;                                                                                                     \
        image_t attachment[n];                                                                                         \
    }

static inline gfx_target_t*
gfx_target_of_color(void* storage, gfx_color_t* color, int width, int height) {
    gfx_target_t* target = storage;
    target->first_row = 0;
    target->count = 1;
    target->attachment[0] = (image_t){color, GFX_PIXEL_FORMAT, width, height, width * sizeof(gfx_color_t)};
    return target;
}

/* Rounded to a word so each carved attachment starts aligned. */
static inline size_t
gfx_attachment_bytes(image_format_t format, int width, int rows) {
    const size_t pitch = ((size_t)width * image_format_bits(format) + CHAR_BIT - 1) / CHAR_BIT;
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
        a->pitch = ((size_t)width * image_format_bits(a->format) + CHAR_BIT - 1) / CHAR_BIT;
        p += gfx_attachment_bytes(a->format, width, height);
    }
    return p;
}

static inline void
gfx_target_window(gfx_target_t* window, const gfx_target_t* target, int row0, int row1) {
    assert(row0 >= target->first_row && row1 >= row0 && row1 <= target->first_row + target->attachment[0].height);
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

static inline void*
gfx_target_row(const gfx_target_t* target, int index, int y) {
    return image_row(&target->attachment[index], y - target->first_row);
}

static inline gfx_color_t*
gfx_target_color(const gfx_target_t* target, int y) {
    const image_t* a = &target->attachment[GFX_ATTACHMENT_COLOR];
    return (gfx_color_t*)((char*)a->pixels + (size_t)(y - target->first_row) * a->pitch);
}

static inline uint16_t*
gfx_target_depth(const gfx_target_t* target, int y) {
    const image_t* a = &target->attachment[GFX_ATTACHMENT_DEPTH];
    return (uint16_t*)((char*)a->pixels + (size_t)(y - target->first_row) * a->pitch);
}

static inline recti_t
gfx_target_clip_rect(const gfx_target_t* target, recti_t clip, recti_t rect) {
    const int first = target->first_row;
    const int height = target->attachment[0].height;
    return recti_from_edges(mathi_max(rect.x, clip.x), mathi_max(mathi_max(rect.y, clip.y), first),
                            mathi_min(recti_right(rect), recti_right(clip)),
                            mathi_min(mathi_min(recti_bottom(rect), recti_bottom(clip)), first + height));
}

/* Whole, word-aligned rows permit two pixels per store. */
static inline void
gfx_target_fill(const gfx_target_t* target, gfx_color_t colour) {
    const uint32_t pair = ((uint32_t)colour << 16) | colour;
    uint32_t* words = target->attachment[0].pixels;
    const size_t count = ((size_t)target->attachment[0].pitch * target->attachment[0].height) / sizeof(uint32_t);
    for (size_t i = 0; i < count; i++) {
        words[i] = pair;
    }
}

static inline bool
gfx_target_row_range_overlaps(const gfx_target_t* target, int y0, int y1) {
    return y0 < target->first_row + target->attachment[0].height && y1 > target->first_row;
}

static inline bool
gfx_target_holds_rows(const gfx_target_t* target, int y0, int y1) {
    return y0 >= target->first_row && y1 <= target->first_row + target->attachment[0].height;
}

static inline recti_t
gfx_target_fill_rect(const gfx_target_t* target, recti_t clip, recti_t shape, gfx_color_t color) {
    const recti_t rect = gfx_target_clip_rect(target, clip, shape);
    char* pixels = target->attachment[0].pixels;
    const size_t pitch = target->attachment[0].pitch;
    const int first = target->first_row;
    for (int row = rect.y; row < recti_bottom(rect); row++) {
        gfx_color_t* dst = (gfx_color_t*)(pixels + (size_t)(row - first) * pitch) + rect.x;
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
 * NULL rows selects exact 2x expansion; target->first_row addresses a strip.
 * A nonzero dark_run shades its latter half, with the supplied keep fraction in RGB565_SCALE_UNIT. */
static inline __attribute__((always_inline)) void
gfx_target_paired_rows_shaded(const gfx_target_t* target, const gfx_target_t* source, uint16_t clear,
                              const uint16_t* rows, int first_row, int row_count, bool doubled, int dark_run,
                              unsigned keep) {
    const uint16_t* color = source->attachment[GFX_ATTACHMENT_COLOR].pixels;
    const uint16_t* depth =
        source->count > GFX_ATTACHMENT_DEPTH ? source->attachment[GFX_ATTACHMENT_DEPTH].pixels : NULL;
    const size_t source_pitch = source->attachment[GFX_ATTACHMENT_COLOR].pitch;
    const size_t target_pitch = target->attachment[GFX_ATTACHMENT_COLOR].pitch;
    char* output = target->attachment[GFX_ATTACHMENT_COLOR].pixels;
    const int row0 = target->first_row;
    const int end = first_row + row_count;
    const int stores = target_pitch / sizeof(uint32_t);
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
        for (int x = 0; x < stores; x++) {
            const uint32_t pair =
                doubled ? 0x10001U * gfx_target_shown_pixel(input, input_depth, x, clear)
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
    gfx_target_paired_rows_shaded(target, source, clear, rows, first_row, row_count, doubled, 0, RGB565_SCALE_UNIT);
}

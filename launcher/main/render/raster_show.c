/*
 * raster_show: development attachments paint colour from depth or their
 * own per-pixel maps before upscale.
 */
#include "render/raster_show.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/r3d_pipeline.h"
#include "render/r3d_span_internal.h"
#include "render/raster.h"

#define GREY_MAX 255

/* The raster's colour and depth, carved once. */
typedef struct {
    uint16_t* color;
    const uint16_t* depth;
    int width, height;
    uint16_t clear;
} picture_t;

typedef struct {
    uint16_t lo, hi; /* the range of drawn depths */
    bool any;        /* whether anything was drawn */
} depth_range_t;

static depth_range_t
drawn_range(const uint16_t* depth, size_t count) {
    depth_range_t r = {R3D_DEPTH_NEAREST, R3D_DEPTH_EMPTY, false};
    for (size_t i = 0; i < count; i++) {
        if (depth[i] == R3D_DEPTH_EMPTY) {
            continue;
        }
        r.any = true;
        r.lo = depth[i] < r.lo ? depth[i] : r.lo;
        r.hi = depth[i] > r.hi ? depth[i] : r.hi;
    }
    return r;
}

/* Nearest white, farthest black, stretched over this frame's own range. A
 * frame of one depth has no range to stretch over and reads as nearest. */
static uint16_t
grey_of(uint16_t depth, const depth_range_t* range) {
    const uint32_t span = (uint32_t)range->hi - range->lo;
    const uint32_t grey = span == 0 ? GREY_MAX : (GREY_MAX * (uint32_t)(depth - range->lo)) / span;
    const int32_t channel = (int32_t)(grey << 8);
    return r3d_span_pack(channel, channel, channel);
}

static uint16_t
colour_of(const picture_t* f, uint16_t depth, const depth_range_t* range) {
    return depth == R3D_DEPTH_EMPTY ? f->clear : grey_of(depth, range);
}

/* The farthest depth in the tile whose first pixel is (x0, y0), or
 * R3D_DEPTH_EMPTY when any of its pixels is empty. */
static uint16_t
tile_farthest(const picture_t* f, int x0, int y0) {
    const int x1 = x0 + RASTER_SHOW_TILE < f->width ? x0 + RASTER_SHOW_TILE : f->width;
    const int y1 = y0 + RASTER_SHOW_TILE < f->height ? y0 + RASTER_SHOW_TILE : f->height;
    uint16_t farthest = R3D_DEPTH_NEAREST;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            const uint16_t d = f->depth[((size_t)y * (size_t)f->width) + (size_t)x];
            if (d == R3D_DEPTH_EMPTY) {
                return R3D_DEPTH_EMPTY;
            }
            farthest = d < farthest ? d : farthest;
        }
    }
    return farthest;
}

static void
fill_tile(const picture_t* f, int x0, int y0, uint16_t colour) {
    const int x1 = x0 + RASTER_SHOW_TILE < f->width ? x0 + RASTER_SHOW_TILE : f->width;
    const int y1 = y0 + RASTER_SHOW_TILE < f->height ? y0 + RASTER_SHOW_TILE : f->height;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            f->color[((size_t)y * (size_t)f->width) + (size_t)x] = colour;
        }
    }
}

static void
show_depth(const picture_t* f, const depth_range_t* range) {
    const size_t count = (size_t)f->width * (size_t)f->height;
    for (size_t i = 0; i < count; i++) {
        f->color[i] = colour_of(f, f->depth[i], range);
    }
}

static void
show_tiles(const picture_t* f, const depth_range_t* range) {
    for (int y = 0; y < f->height; y += RASTER_SHOW_TILE) {
        for (int x = 0; x < f->width; x += RASTER_SHOW_TILE) {
            fill_tile(f, x, y, colour_of(f, tile_farthest(f, x, y), range));
        }
    }
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_render_target_t* target, bool tiles) {
    (void)self;
    const picture_t picture = {gfx_render_target_color(target, 0), gfx_render_target_depth(target, 0), raster->width,
                               raster->height, raster->clear};
    const size_t count = (size_t)picture.width * (size_t)picture.height;
    const depth_range_t range = drawn_range(picture.depth, count);
    if (!range.any) {
        for (size_t i = 0; i < count; i++) {
            picture.color[i] = picture.clear;
        }
    } else if (!tiles) {
        show_depth(&picture, &range);
    } else {
        show_tiles(&picture, &range);
    }
}

static void
depth_show(const raster_attachment_t* self, const raster_t* raster, const gfx_render_target_t* target, int index) {
    (void)index;
    show(self, raster, target, false);
}

static void
tiles_show(const raster_attachment_t* self, const raster_t* raster, const gfx_render_target_t* target, int index) {
    (void)index;
    show(self, raster, target, true);
}

raster_attachment_t
raster_depth_view(void* state) {
    (void)state;
    return (raster_attachment_t){.show = depth_show};
}

raster_attachment_t
raster_depth_tiles_view(void* state) {
    (void)state;
    return (raster_attachment_t){.show = tiles_show};
}

void
raster_show_map(const gfx_render_target_t* picture, int index, uint16_t clear,
                gfx_color_t (*color_of)(const void* pixel, uint16_t clear)) {
    for (int y = picture->row0; y < picture->row1; y++) {
        const char* pixels = gfx_render_target_row(picture, index, y);
        gfx_color_t* color = gfx_render_target_color(picture, y);
        for (int x = 0; x < picture->width; x++) {
            color[x] = color_of(pixels, clear);
            pixels += picture->attachment[index].bytes_per_pixel;
        }
    }
}

void
raster_show(const raster_t* raster) {
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    for (int k = 0; k < raster->attachment_count; k++) {
        const raster_attachment_t* a = raster->attachments[k];
        if (a->show != NULL) {
            a->show(a, raster, &b.picture, GFX_ATTACHMENT_FURTHER + k);
        }
    }
}

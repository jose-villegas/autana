/*
 * raster_show: the raster's development view modes, drawing its depth, or
 * each tile's farthest depth, in place of its colour.
 */
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

void
raster_show(const raster_t* raster, raster_show_t mode) {
    if (mode == RASTER_SHOW_SHADED) {
        return;
    }
    const r3d_pipeline_buffers_t b = r3d_pipeline_carve(raster);
    const picture_t picture = {b.color, b.depth, raster->width, raster->height, raster->clear};
    const size_t count = (size_t)picture.width * (size_t)picture.height;
    const depth_range_t range = drawn_range(picture.depth, count);
    if (!range.any) {
        for (size_t i = 0; i < count; i++) {
            picture.color[i] = picture.clear;
        }
    } else if (mode == RASTER_SHOW_DEPTH) {
        show_depth(&picture, &range);
    } else {
        show_tiles(&picture, &range);
    }
}

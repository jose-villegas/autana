#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "render/r3d_lit_frame.h"

#define GREY_MAX 255

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
colour_of(const r3d_lit_frame_t* f, uint16_t depth, const depth_range_t* range) {
    return depth == R3D_DEPTH_EMPTY ? f->clear : grey_of(depth, range);
}

/* The farthest depth in the tile whose first pixel is (x0, y0), or
 * R3D_DEPTH_EMPTY when any of its pixels is empty. */
static uint16_t
tile_farthest(const r3d_lit_frame_t* f, int x0, int y0) {
    const int x1 = x0 + R3D_LIT_TILE < f->width ? x0 + R3D_LIT_TILE : f->width;
    const int y1 = y0 + R3D_LIT_TILE < f->height ? y0 + R3D_LIT_TILE : f->height;
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
fill_tile(const r3d_lit_frame_t* f, int x0, int y0, uint16_t colour) {
    const int x1 = x0 + R3D_LIT_TILE < f->width ? x0 + R3D_LIT_TILE : f->width;
    const int y1 = y0 + R3D_LIT_TILE < f->height ? y0 + R3D_LIT_TILE : f->height;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            f->color[((size_t)y * (size_t)f->width) + (size_t)x] = colour;
        }
    }
}

static void
show_depth(const r3d_lit_frame_t* f, const depth_range_t* range) {
    const size_t count = (size_t)f->width * (size_t)f->height;
    for (size_t i = 0; i < count; i++) {
        f->color[i] = colour_of(f, f->depth[i], range);
    }
}

static void
show_tiles(const r3d_lit_frame_t* f, const depth_range_t* range) {
    for (int y = 0; y < f->height; y += R3D_LIT_TILE) {
        for (int x = 0; x < f->width; x += R3D_LIT_TILE) {
            fill_tile(f, x, y, colour_of(f, tile_farthest(f, x, y), range));
        }
    }
}

void
r3d_lit_frame_show(const r3d_lit_frame_t* frame, r3d_lit_view_mode_t mode) {
    if (mode == R3D_LIT_VIEW_SHADED) {
        return;
    }
    const size_t count = (size_t)frame->width * (size_t)frame->height;
    const depth_range_t range = drawn_range(frame->depth, count);
    if (!range.any) {
        for (size_t i = 0; i < count; i++) {
            frame->color[i] = frame->clear;
        }
    } else if (mode == R3D_LIT_VIEW_DEPTH) {
        show_depth(frame, &range);
    } else {
        show_tiles(frame, &range);
    }
}

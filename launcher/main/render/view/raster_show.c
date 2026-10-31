/*
 * raster_show: development attachments paint colour from depth or their
 * own per-pixel maps before upscale.
 */
#include "render/view/raster_show.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profile/frame_cost.h"
#include "render/r3d_span_internal.h"
#include "render/raster_internal.h"

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
colour_of(uint16_t clear, uint16_t depth, const depth_range_t* range) {
    return depth == R3D_DEPTH_EMPTY ? clear : grey_of(depth, range);
}

/* The farthest depth in the tile whose first pixel is (x0, y0), or
 * R3D_DEPTH_EMPTY when any of its pixels is empty. */
static uint16_t
tile_farthest(const gfx_target_t* f, int x0, int y0) {
    const int x1 = x0 + RASTER_SHOW_TILE < f->attachment[0].width ? x0 + RASTER_SHOW_TILE : f->attachment[0].width;
    const int y1 = y0 + RASTER_SHOW_TILE < f->attachment[0].height ? y0 + RASTER_SHOW_TILE : f->attachment[0].height;
    uint16_t farthest = R3D_DEPTH_NEAREST;
    for (int y = y0; y < y1; y++) {
        const uint16_t* depth = gfx_target_depth(f, f->first_row + y);
        for (int x = x0; x < x1; x++) {
            const uint16_t d = depth[x];
            if (d == R3D_DEPTH_EMPTY) {
                return R3D_DEPTH_EMPTY;
            }
            farthest = d < farthest ? d : farthest;
        }
    }
    return farthest;
}

static void
fill_tile(const gfx_target_t* f, int x0, int y0, uint16_t colour) {
    const int x1 = x0 + RASTER_SHOW_TILE < f->attachment[0].width ? x0 + RASTER_SHOW_TILE : f->attachment[0].width;
    const int y1 = y0 + RASTER_SHOW_TILE < f->attachment[0].height ? y0 + RASTER_SHOW_TILE : f->attachment[0].height;
    for (int y = y0; y < y1; y++) {
        gfx_color_t* color = gfx_target_color(f, f->first_row + y);
        for (int x = x0; x < x1; x++) {
            color[x] = colour;
        }
    }
}

static void
show_depth(const gfx_target_t* f, uint16_t clear, const depth_range_t* range) {
    for (int y = f->first_row; y < gfx_target_end_row(f); y++) {
        uint16_t* color = gfx_target_color(f, y);
        const uint16_t* depth = gfx_target_depth(f, y);
        for (int x = 0; x < f->attachment[0].width; x++) {
            color[x] = colour_of(clear, depth[x], range);
        }
    }
}

static void
show_tiles(const gfx_target_t* f, uint16_t clear, const depth_range_t* range) {
    for (int y = 0; y < f->attachment[0].height; y += RASTER_SHOW_TILE) {
        for (int x = 0; x < f->attachment[0].width; x += RASTER_SHOW_TILE) {
            fill_tile(f, x, y, colour_of(clear, tile_farthest(f, x, y), range));
        }
    }
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* target, bool tiles) {
    (void)self;
    const size_t count = (size_t)target->attachment[0].width * target->attachment[0].height;
    const depth_range_t range = drawn_range(gfx_target_depth(target, target->first_row), count);
    if (!range.any) {
        gfx_target_fill(target, raster->clear);
    } else if (!tiles) {
        show_depth(target, raster->clear, &range);
    } else {
        show_tiles(target, raster->clear, &range);
    }
}

static void
depth_show(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* target, int index) {
    (void)index;
    show(self, raster, target, false);
}

static void
tiles_show(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* target, int index) {
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
raster_show_map(const gfx_target_t* picture, int index, uint16_t clear,
                gfx_color_t (*color_of)(const void* pixel, uint16_t clear)) {
    const image_t* map = &picture->attachment[index];
    const image_t* output = &picture->attachment[GFX_ATTACHMENT_COLOR];
    const size_t step = image_format_bits(map->format) / CHAR_BIT;
    const int width = output->width;
    const int height = output->height;
    const size_t map_pitch = map->pitch;
    const size_t color_pitch = output->pitch;
    const char* row = map->pixels;
    char* out = output->pixels;
    for (int y = 0; y < height; y++, row += map_pitch, out += color_pitch) {
        const char* pixels = row;
        gfx_color_t* color = (gfx_color_t*)out;
        for (int x = 0; x < width; x++) {
            color[x] = color_of(pixels, clear);
            pixels += step;
        }
    }
}

void
raster_show(const raster_t* raster) {
    FRAME_COST_BEGIN(shown_from);
    const raster_buffers_t b = raster_carve(raster);
    for (int k = 0; k < raster->attachment_count; k++) {
        const raster_attachment_t* a = raster->attachments[k];
        if (a->show != NULL) {
            a->show(a, raster, &b.picture, GFX_ATTACHMENT_FURTHER + k);
        }
    }
    FRAME_COST_END(shown_from, "r3d.show");
}

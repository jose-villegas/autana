#include "render/view/raster_overdraw.h"

#include <string.h>

#include "image/pixel/rgb565.h"
#include "image/pixel/rgb888.h"
#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/view/raster_show.h"

_Static_assert(sizeof(raster_overdraw_px_t) == sizeof(uint16_t), "a pixel is one IMAGE_U16 map entry");

/* The ramp runs from blue, one layer, to red at RASTER_OVERDRAW_RAMP_LAYERS. */
#define RAMP_FIRST_HUE (RGB888_HUE_TURN * 2 / 3)

static void
clear(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(raster_overdraw_px_t));
}

static void
count_span(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int x_first, int x_last, int32_t z,
           int32_t dz) {
    const uint16_t* depth = gfx_target_depth(rows, y);
    raster_overdraw_px_t* layers = gfx_target_row(rows, writer->attachment, y);
    for (int x = x_first; x <= x_last; x++, z += dz) {
        raster_overdraw_px_t* px = &layers[x];
        px->tested += px->tested < UINT8_MAX;
        px->written += px->written < UINT8_MAX && (uint16_t)(z >> R3D_DEPTH_SHIFT) == depth[x];
    }
}

static bool
writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    (void)self;
    (void)instance;
    *out = (r3d_span_writer_t){.span = count_span};
    return true;
}

static gfx_color_t
pixel_color(const void* pixel, uint16_t clear_color) {
    const int tested = ((const raster_overdraw_px_t*)pixel)->tested;
    if (tested == 0) {
        return clear_color;
    }
    const int step = (tested < RASTER_OVERDRAW_RAMP_LAYERS ? tested : RASTER_OVERDRAW_RAMP_LAYERS) - 1;
    return RGB565BE(rgb888_hue(RAMP_FIRST_HUE - (RAMP_FIRST_HUE * step / (RASTER_OVERDRAW_RAMP_LAYERS - 1))));
}

static void
total(raster_overdraw_t* state, const gfx_target_t* picture, int index) {
    raster_overdraw_frame_t f = {0};
    for (int y = picture->first_row; y < gfx_target_end_row(picture); y++) {
        const raster_overdraw_px_t* layers = gfx_target_row(picture, index, y);
        for (int x = 0; x < picture->attachment[0].width; x++) {
            const raster_overdraw_px_t px = layers[x];
            if (px.tested == 0) {
                continue;
            }
            f.pixels++;
            f.tested += px.tested;
            f.written += px.written;
            f.tested_max = px.tested > f.tested_max ? px.tested : f.tested_max;
            f.written_max = px.written > f.written_max ? px.written : f.written_max;
        }
    }
    state->frame = f;
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* picture, int index) {
    total(self->state, picture, index);
    raster_show_map(picture, index, raster->clear, pixel_color);
}

raster_attachment_t
raster_overdraw_view(void* state) {
    return (raster_attachment_t){IMAGE_U16, clear, NULL, writer, NULL, show, state};
}

raster_overdraw_frame_t
raster_overdraw_frame(const raster_overdraw_t* state) {
    return state->frame;
}

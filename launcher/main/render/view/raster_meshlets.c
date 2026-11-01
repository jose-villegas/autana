#include "render/view/raster_meshlets.h"

#include <assert.h>
#include <string.h>

#include "image/pixel/rgb565.h"
#include "image/pixel/rgb888.h"
#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/view/raster_show.h"

static void
clear(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(uint16_t));
}

static void
begin(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view) {
    (void)view;
    raster_meshlets_t* state = self->state;
    state->next = 1;
    state->raster = raster;
}

static bool
writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    raster_meshlets_t* state = self->state;
    const uint32_t clusters = (uint32_t)state->raster->instances[instance].mesh->cluster_count;
    assert(state->next > 0 && state->next + clusters <= (uint32_t)UINT16_MAX + 1);
    *out = (r3d_span_writer_t){.span = raster_attachment_tag, .step = R3D_SPAN_STEP_CLUSTER, .value = state->next};
    state->next += clusters;
    return true;
}

gfx_color_t
raster_meshlets_color(uint32_t id) {
    const uint32_t hash = id * 2654435761U;
    return RGB565BE(rgb888_hue((int)(((uint64_t)hash * RGB888_HUE_TURN) >> 32)));
}

static gfx_color_t
pixel_color(const void* pixel, uint16_t clear_color) {
    const uint16_t id = *(const uint16_t*)pixel;
    return id == 0 ? clear_color : raster_meshlets_color(id);
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* picture, int index) {
    (void)self;
    raster_show_map(picture, index, raster->clear, pixel_color);
}

raster_attachment_t
raster_meshlets_view(void* state) {
    return (raster_attachment_t){IMAGE_U16, clear, begin, writer, NULL, show, state};
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
bool
raster_meshlets_copy_view(void* destination, const void* source, const raster_t* raster) {
    raster_meshlets_t* out = destination;
    const raster_meshlets_t* in = source;
    *out = *in;
    out->raster = in->raster == NULL ? NULL : raster;
    return true;
}
#endif

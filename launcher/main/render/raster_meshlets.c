#include "render/raster_meshlets.h"

#include <assert.h>
#include <string.h>

#include "render/raster.h"
#include "render/raster_show.h"

static void
clear(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(uint16_t));
}

static void
begin(const raster_attachment_t* self, const raster_t* raster, const camera_t* camera, int quarter) {
    (void)camera;
    (void)quarter;
    raster_meshlets_t* state = self->state;
    uint32_t clusters = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        clusters += (uint32_t)raster->instances[i].mesh->cluster_count;
        assert(clusters <= UINT16_MAX);
    }
    state->next = 1;
    state->raster = raster;
}

static bool
writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    raster_meshlets_t* state = self->state;
    const uint32_t clusters = (uint32_t)state->raster->instances[instance].mesh->cluster_count;
    assert(state->next > 0 && state->next + clusters <= (uint32_t)UINT16_MAX + 1);
    *out = (r3d_span_writer_t){.span = raster_attachment_tag, .per_cluster = true, .value = state->next};
    state->next += clusters;
    return true;
}

static gfx_color_t
pixel_color(const void* pixel, uint16_t clear_color) {
    const uint16_t id = *(const uint16_t*)pixel;
    const uint32_t hash = (uint32_t)id * 2654435761U;
    const uint32_t rgb = gfx_hue_rgb((int)(((uint64_t)hash * GFX_HUE_TURN) >> 32));
    return id == 0 ? clear_color : GFX_RGB(rgb);
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_render_target_t* picture, int index) {
    (void)self;
    raster_show_map(picture, index, raster->clear, pixel_color);
}

raster_attachment_t
raster_meshlets_view(void* state) {
    return (raster_attachment_t){sizeof(uint16_t), clear, begin, writer, NULL, show, state};
}

#include "render/view/raster_meshlets.h"

#include <assert.h>

#include "image/pixel/rgb565.h"
#include "image/pixel/rgb888.h"
#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/view/raster_show.h"

static void
begin(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view) {
    (void)view;
    raster_meshlets_t* state = self->state;
    state->next = 1;
    state->raster = raster;
}

static bool
writer(const raster_attachment_t* self, int entry, r3d_span_writer_t* out) {
    raster_meshlets_t* state = self->state;
    const r3d_span_step_t step = self->format == IMAGE_U32 ? R3D_SPAN_STEP_TRIANGLE : R3D_SPAN_STEP_CLUSTER;
    const r3d_lit_mesh_t* mesh = raster_drawn_at(state->raster, entry)->mesh;
    const uint32_t count = (uint32_t)(step == R3D_SPAN_STEP_TRIANGLE ? mesh->triangle_count : mesh->cluster_count);
    const uint64_t limit = self->format == IMAGE_U16 ? (uint64_t)UINT16_MAX + 1 : UINT32_MAX;
    assert(state->next > 0 && (uint64_t)state->next + count <= limit);
    *out = (r3d_span_writer_t){.span = raster_attachment_tag, .step = step, .value = state->next};
    state->next += count;
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

static gfx_color_t
triangle_color(const void* pixel, uint16_t clear_color) {
    const uint32_t id = *(const uint32_t*)pixel;
    return id == 0 ? clear_color : raster_meshlets_color(id);
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* picture, int index) {
    raster_show_map(picture, index, raster->clear, self->format == IMAGE_U32 ? triangle_color : pixel_color);
}

raster_attachment_t
raster_meshlets_view(void* state) {
    return raster_attachment_ids(begin, writer, show, state);
}

raster_attachment_t
raster_triangles_view(void* state) {
    return raster_attachment_ids_u32(begin, writer, show, state);
}

#include "render/view/raster_triangles.h"
#include "render/view/raster_meshlets.h"

#include <assert.h>

#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/view/raster_show.h"

static void
begin(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view) {
    (void)view;
    raster_triangles_t* state = self->state;
    state->next = 1;
    state->raster = raster;
}

static bool
writer(const raster_attachment_t* self, int entry, r3d_span_writer_t* out) {
    raster_triangles_t* state = self->state;
    const uint32_t triangles = (uint32_t)raster_drawn_at(state->raster, entry)->mesh->triangle_count;
    assert(state->next > 0 && triangles <= UINT32_MAX - state->next);
    *out = (r3d_span_writer_t){.span = raster_attachment_tag, .step = R3D_SPAN_STEP_TRIANGLE, .value = state->next};
    state->next += triangles;
    return true;
}

static gfx_color_t
pixel_color(const void* pixel, uint16_t clear_color) {
    const uint32_t id = *(const uint32_t*)pixel;
    return id == 0 ? clear_color : raster_meshlets_color(id);
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* picture, int index) {
    (void)self;
    raster_show_map(picture, index, raster->clear, pixel_color);
}

raster_attachment_t
raster_triangles_view(void* state) {
    return raster_attachment_ids_u32(begin, writer, show, state);
}

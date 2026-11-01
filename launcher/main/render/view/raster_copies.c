#include "render/view/raster_copies.h"

#include <assert.h>
#include <string.h>

#include "render/r3d_span.h"
#include "render/view/raster_meshlets.h"
#include "render/view/raster_show.h"

int
raster_copies_shape(const raster_t* raster, int i, int* copies) {
    int first = -1;
    *copies = 0;
    for (int j = 0; j < raster->instance_count; j++) {
        if (raster->instances[j].mesh->positions == raster->instances[i].mesh->positions) {
            first = first < 0 ? j : first;
            ++*copies;
        }
    }
    return first;
}

static void
clear(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(uint16_t));
}

static void
begin(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view) {
    (void)view;
    raster_copies_t* state = self->state;
    assert(raster->instance_count < RASTER_COPIES_UNIQUE);
    state->raster = raster;
}

/* The shape's first instance plus one for a copy, so 0 stays empty. */
static bool
writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    const raster_copies_t* state = self->state;
    int copies;
    const int shape = raster_copies_shape(state->raster, instance, &copies);
    const uint32_t id = copies >= RASTER_COPIES_SHARED ? 1U + (uint32_t)shape : RASTER_COPIES_UNIQUE;
    *out = (r3d_span_writer_t){.span = raster_attachment_tag, .step = R3D_SPAN_STEP_NONE, .value = id};
    return true;
}

/* A copy in its boxes-view hue, so a shape matches its boxes there. */
static gfx_color_t
pixel_color(const void* pixel, uint16_t clear_color) {
    const uint16_t id = *(const uint16_t*)pixel;
    return id == 0 ? clear_color : id == RASTER_COPIES_UNIQUE ? RASTER_COPIES_GREY : raster_meshlets_color(id);
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* picture, int index) {
    (void)self;
    raster_show_map(picture, index, raster->clear, pixel_color);
}

raster_attachment_t
raster_copies_view(void* state) {
    return (raster_attachment_t){IMAGE_U16, clear, begin, writer, NULL, show, state};
}

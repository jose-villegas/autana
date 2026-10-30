#include "render/view/raster_boxes.h"

#include "render/r3d_pipeline.h"
#include "render/view/raster_lines.h"
#include "render/view/raster_meshlets.h"

static void
begin(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view) {
    raster_boxes_t* s = self->state;
    s->raster = raster;
    s->camera = *view;
}

raster_attachment_t
raster_boxes_view(void* state) {
    return (raster_attachment_t){.begin = begin, .state = state};
}

void
raster_boxes_set_objects(raster_boxes_t* state, const raster_box_t* boxes, int count) {
    state->objects = boxes;
    state->object_count = count;
}

/* The shape of instance `i`: the first instance drawing the same mesh. */
static int
instance_shape(const raster_t* raster, int i) {
    int first = 0;
    while (raster->instances[first].mesh != raster->instances[i].mesh) {
        first++;
    }
    return first;
}

void
raster_boxes_overlay(void* state, int width, int height) {
    raster_boxes_t* s = state;
    if (s->raster == NULL) {
        return;
    }
    const raster_t* raster = s->raster;
    render_view_t seen = s->camera;
    render_view_refit(&seen, (viewport_t){width, height, seen.viewport.quarter});
    raster_lines_depth_t depth;
    raster_lines_depth(&depth, raster, width, height);
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_instance_t* instance = &raster->instances[i];
        r3d_lens_t lens;
        r3d_lens_init(&lens, &seen, instance->mesh->position_scale);
        if (instance->model != NULL) {
            r3d_lens_place(&lens, instance->model, instance->mesh->position_scale);
        }
        const gfx_color_t color = raster_meshlets_color(1U + (uint32_t)instance_shape(raster, i));
        raster_lines_box(&lens, &depth, box3f_from_box3s(instance->mesh->nodes[0].bounds, 1.0F), color);
    }
    r3d_lens_t world;
    r3d_lens_init(&world, &seen, 1);
    for (int i = 0; i < s->object_count; i++) {
        raster_lines_box(&world, &depth, s->objects[i].box, raster_meshlets_color(1U + s->objects[i].shape));
    }
}

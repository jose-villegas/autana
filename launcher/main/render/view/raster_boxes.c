#include "render/view/raster_boxes.h"

#include "core/tune.h"
#include "render/r3d_pipeline.h"
#include "render/view/raster_lines.h"
#include "render/view/raster_meshlets.h"

TUNE_OWNER(boxview);
TUNE(boxview, copies, RASTER_BOXES_COPIES_DEFAULT, 1, UINT16_MAX);
TUNE(boxview, behind, RASTER_BOXES_BEHIND_HIDE, 0, RASTER_BOXES_BEHIND_COUNT - 1);

/* What a stretch behind keeps under each boxview.behind but full, which tests nothing. */
static const unsigned BEHIND_KEEP[RASTER_BOXES_BEHIND_COUNT] = {
    [RASTER_BOXES_BEHIND_HIDE] = 0,
    [RASTER_BOXES_BEHIND_DIM] = RASTER_LINES_BEHIND_KEEP,
};

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

/* The first instance drawing instance `i`'s mesh, and how many instances draw
 * it. Each renderer holds its own view of a mesh, so copies are the instances
 * whose views map the same positions. A skinned or dynamic mesh writes its
 * own positions, so it is a shape of its own. */
static int
instance_shape(const raster_t* raster, int i, int* copies) {
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
    raster_lines_depth(&depth, raster, width, height, BEHIND_KEEP[behind]);
    const raster_lines_depth_t* tested = behind == RASTER_BOXES_BEHIND_FULL ? NULL : &depth;
    for (int i = 0; i < raster->instance_count; i++) {
        int shape_copies;
        const int shape = instance_shape(raster, i, &shape_copies);
        if (shape_copies < copies) {
            continue;
        }
        const r3d_instance_t* instance = &raster->instances[i];
        r3d_lens_t lens;
        r3d_lens_init(&lens, &seen, instance->mesh->position_scale);
        if (instance->model != NULL) {
            r3d_lens_place(&lens, instance->model, instance->mesh->position_scale);
        }
        const gfx_color_t color = raster_meshlets_color(1U + (uint32_t)shape);
        raster_lines_box(&lens, tested, box3f_from_box3s(instance->mesh->nodes[0].bounds, 1.0F), color);
    }
}

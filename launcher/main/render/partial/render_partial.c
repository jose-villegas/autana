#include "render/partial/render_partial.h"

#include <math.h>
#include <string.h>

#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_project.h"
#include "render/raster_internal.h"

/* Rows each side of a box's own, for a vertex snapped to the next row and a
 * small fill's spill past its box. */
#define RENDER_PARTIAL_ROW_MARGIN 2

void
render_partial_set_changed(render_partial_t* partial, const box3f_t* boxes, int count) {
    if (count < 0 || count > RENDER_PARTIAL_CHANGES_MAX) {
        partial->changed_count = -1;
        return;
    }
    memcpy(partial->changed, boxes, sizeof(*boxes) * (size_t)count);
    partial->changed_count = count;
}

/* Field by field: a view's padding is not part of what it shows. */
static bool
same_view(const render_view_t* a, const render_view_t* b) {
    return memcmp(&a->position, &b->position, sizeof a->position) == 0
           && memcmp(&a->screen_x, &b->screen_x, sizeof a->screen_x) == 0
           && memcmp(&a->screen_y, &b->screen_y, sizeof a->screen_y) == 0
           && memcmp(&a->forward, &b->forward, sizeof a->forward) == 0
           && memcmp(&a->lens, &b->lens, sizeof a->lens) == 0 && a->viewport.width == b->viewport.width
           && a->viewport.height == b->viewport.height && a->viewport.quarter == b->viewport.quarter
           && a->present == b->present && a->small_fill.ceiling == b->small_fill.ceiling
           && a->small_fill.gate == b->small_fill.gate && a->small_fill.style == b->small_fill.style
           && a->skip_hidden == b->skip_hidden && memcmp(&a->shift, &b->shift, sizeof a->shift) == 0;
}

#define SAME_FIELD(field, type) &&a.field == b.field

static bool
same_settings(render_settings_t a, render_settings_t b) {
    return true RENDER_SETTINGS_FIELDS(SAME_FIELD);
}

/* Whether the kept picture is this one's but for the changed boxes. */
static bool
can_draw_over(const render_partial_t* p, const raster_t* raster, const render_view_t* view) {
    return raster_settings(raster).partial_redraw && p->kept && p->changed_count >= 0 && raster->attachment_count == 0
           && raster->width == p->width && raster->height == p->height && raster->instance_count == p->instance_count
           && raster->clear == p->clear && raster->upscale.filter == p->filter
           && same_settings(raster_settings(raster), p->settings) && same_view(view, &p->view);
}

/* Rows [first, end) `box` covers in the raster, or false when a corner is
 * at or behind the near plane. */
static bool
box_rows(const r3d_lens_t* lens, box3f_t box, int* first, int* end) {
    float top = INFINITY;
    float bottom = -INFINITY;
    for (int k = 0; k < BOX3_CORNERS; k++) {
        const vec3f_t l = mat4f_apply(&lens->m, box3f_corner(box, k));
        if (!(l.z > lens->project.near_z)) {
            return false;
        }
        const float y = lens->project.cy + (lens->project.fy * -l.y / l.z);
        top = mathf_min(top, y);
        bottom = mathf_max(bottom, y);
    }
    /* Held in an int's range before the cast: a corner near the eye's plane projects far. */
    *first = (int)floorf(mathf_max(top, -R3D_PIXEL_LIMIT)) - RENDER_PARTIAL_ROW_MARGIN;
    *end = (int)floorf(mathf_min(bottom, R3D_PIXEL_LIMIT)) + 1 + RENDER_PARTIAL_ROW_MARGIN;
    return true;
}

render_rows_t
render_partial_rows(render_partial_t* p, const raster_t* raster, const render_view_t* view) {
    const render_rows_t every = {0, raster->height};
    if (!can_draw_over(p, raster, view)) {
        return every;
    }
    r3d_lens_t lens;
    raster_lens(raster, view, 1, &lens);
    int first = raster->height;
    int end = 0;
    for (int i = 0; i < p->changed_count; i++) {
        int box_first;
        int box_end;
        if (!box_rows(&lens, p->changed[i], &box_first, &box_end)) {
            return every;
        }
        first = mathi_min(first, mathi_max(box_first, 0));
        end = mathi_max(end, mathi_min(box_end, raster->height));
    }
    return first < end ? (render_rows_t){first, end} : (render_rows_t){0, 0};
}

void
render_partial_kept(render_partial_t* p, const raster_t* raster, const render_view_t* view) {
    /* A debug view drew its own colours. */
    p->kept = raster_settings(raster).partial_redraw && raster->attachment_count == 0;
    p->view = *view;
    p->width = raster->width;
    p->height = raster->height;
    p->instance_count = raster->instance_count;
    p->clear = raster->clear;
    p->filter = raster->upscale.filter;
    p->settings = raster_settings(raster);
}

void
render_partial_forget(render_partial_t* p) {
    p->kept = false;
}

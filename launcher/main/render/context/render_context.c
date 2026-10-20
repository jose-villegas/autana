#include "render/context/render_context.h"

#include <assert.h>
#include <string.h>

#include "build/build_variant.h"
#include "core/memory.h"
#include "core/timing.h"

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
#include "render/raster_meshlets.h"
#include "render/raster_motion.h"
#include "render/raster_show.h"

static const render_debug_view_t views[] = {
    {"depth", 0, raster_depth_view},
    {"tiles", 0, raster_depth_tiles_view},
    {"motion", sizeof(raster_motion_t), raster_motion_view},
    {"meshlets", sizeof(raster_meshlets_t), raster_meshlets_view},
};
_Static_assert(RENDER_DEBUG_VIEW_COUNT == sizeof views / sizeof views[0], "view table length");
#endif

const render_debug_view_t*
render_context_debug_view(int view) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    return view > RENDER_DEBUG_VIEW_SHADED && view <= RENDER_DEBUG_VIEW_COUNT ? &views[view - 1] : NULL;
#else
    (void)view;
    return NULL;
#endif
}

int
render_context_debug_view_named(const char* name) {
    if (strcmp(name, "shaded") == 0) {
        return RENDER_DEBUG_VIEW_SHADED;
    }
    for (int i = 1; i <= RENDER_DEBUG_VIEW_COUNT; i++) {
        const render_debug_view_t* row = render_context_debug_view(i);
        if (row != NULL && strcmp(name, row->name) == 0) {
            return i;
        }
    }
    return RENDER_DEBUG_VIEW_UNKNOWN;
}

void
render_context_print_debug_views(FILE* out) {
    (void)fputs("shaded", out);
    for (int i = 1; i <= RENDER_DEBUG_VIEW_COUNT; i++) {
        const render_debug_view_t* row = render_context_debug_view(i);
        if (row != NULL) {
            (void)fprintf(out, ", %s", row->name);
        }
    }
}

static render_context_t main_context = {
    .scale_percent = RENDER_CONTEXT_DEFAULT_SCALE_PERCENT,
    .frame = {.step = -1},
};

render_context_t*
render_context_main(void) {
    return &main_context;
}

void
render_context_release(render_context_t* c) {
    memory_free(c->scratch);
    memory_free(c->band_scratch);
    memory_free(c->debug_view_state);
    *c = (render_context_t){
        .scale_percent = RENDER_CONTEXT_DEFAULT_SCALE_PERCENT,
        .frame = {.step = -1},
    };
}

void
render_context_forget(render_context_t* c) {
    raster_split_forget(&c->split);
}

void
render_context_set_scale(render_context_t* c, int percent) {
    assert(percent > 0 && percent <= 100);
    c->scale_percent = percent;
}

void
render_context_set_band_rows(render_context_t* c, int rows) {
    assert(rows >= 0);
    c->band_rows = rows;
}

void
render_context_set_dynamic_resolution(render_context_t* c, const resolution_config_t* config,
                                      const resolution_model_t* model, int first_step) {
    if (config == NULL) {
        c->policy = RENDER_FIXED;
        return;
    }
    c->ladder = *config;
    c->policy = model == NULL ? RENDER_STEPPED : RENDER_PREDICTED;
    if (c->policy == RENDER_STEPPED) {
        resolution_control_init(&c->control, &c->ladder, first_step);
    } else {
        resolution_predict_init(&c->predict, &c->ladder, model, first_step);
    }
}

void
render_context_set_debug_view(render_context_t* c, int view) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    const render_debug_view_t* row = render_context_debug_view(view);
    if (view != RENDER_DEBUG_VIEW_SHADED && row == NULL) {
        return;
    }
    if (c->debug_view == view && (view == RENDER_DEBUG_VIEW_SHADED || c->raster.attachment_count != 0)) {
        return;
    }
    memory_free(c->debug_view_state);
    c->debug_view_state = NULL;
    c->debug_view = RENDER_DEBUG_VIEW_SHADED;
    c->raster.attachment_count = 0;
    c->raster.attachments = NULL;
    if (row == NULL) {
        return;
    }
    if (row->state_bytes != 0) {
        c->debug_view_state = memory_alloc(row->state_bytes, MEMORY_PSRAM);
        if (c->debug_view_state == NULL) {
            return;
        }
        memset(c->debug_view_state, 0, row->state_bytes);
    }
    c->debug_view_attachment = row->attachment(c->debug_view_state);
    c->debug_view_attached[0] = &c->debug_view_attachment;
    c->raster.attachments = c->debug_view_attached;
    c->raster.attachment_count = 1;
    c->debug_view = view;
#else
    (void)c;
    (void)view;
#endif
}

/* The step the policy last chose, or -1 at the fixed scale. */
static int
current_step(const render_context_t* c) {
    if (c->policy == RENDER_FIXED) {
        return -1;
    }
    return c->policy == RENDER_STEPPED ? c->control.step : c->predict.step;
}

/* The size a frame draws at, from a step or the fixed scale. Bands draw at
 * most RENDER_CONTEXT_BAND_PERCENT of the destination on each axis, which
 * keeps the band picture a small block of internal RAM. */
static resolution_step_t
drawn_size(const render_context_t* c, resolution_step_t size, int destination_width, int destination_height) {
    if (c->band_rows != 0) {
        size.width = mathi_min(size.width, destination_width * RENDER_CONTEXT_BAND_PERCENT / 100);
        size.height = mathi_min(size.height, destination_height * RENDER_CONTEXT_BAND_PERCENT / 100);
    }
    return size;
}

/* The source rows a band of `rows` destination rows can read at the
 * raster's size: the nearest-neighbour row map can straddle one more. */
static int
band_source_rows(const raster_t* r, int rows) {
    return rows == 0 ? 0 : (((rows * r->height) + r->destination_height - 1) / r->destination_height) + 1;
}

/* Grows `*block` to at least `needed` bytes of `kind`; false when it cannot. */
static bool
fit_block(void** block, size_t* bytes, size_t needed, memory_kind_t kind) {
    if (needed > *bytes) {
        memory_free(*block);
        *block = memory_alloc(needed, kind);
        *bytes = *block == NULL ? 0 : needed;
    }
    return *block != NULL;
}

/* Dynamic resolution needs the finest step's scratch so a step never
 * allocates. A band picture is the one block in internal RAM. */
static bool
fit_scratch(render_context_t* c) {
    raster_t* r = &c->raster;
    const int width = r->width;
    const int height = r->height;
    if (c->policy != RENDER_FIXED) {
        const resolution_step_t finest = drawn_size(c, c->ladder.steps[0], r->destination_width, r->destination_height);
        r->width = finest.width;
        r->height = finest.height;
    }
    r->band_rows = band_source_rows(r, c->band_rows);
    const size_t needed = raster_scratch_bytes(r);
    const size_t band_needed = r->band_rows == 0 ? 0 : raster_band_bytes(r);
    r->width = width;
    r->height = height;
    r->band_rows = band_source_rows(r, c->band_rows);
    const bool fits =
        fit_block(&c->scratch, &c->scratch_bytes, needed, MEMORY_PSRAM)
        && (band_needed == 0 || fit_block(&c->band_scratch, &c->band_scratch_bytes, band_needed, MEMORY_INTERNAL));
    r->scratch = c->scratch;
    r->band_scratch = c->band_scratch;
    return fits;
}

/* Development views paint the picture's colour from their attachment. */
static void
show(const raster_t* r) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (r->attachment_count != 0) {
        raster_show(r);
    }
#else
    (void)r;
#endif
}

bool
render_context_draw(render_context_t* c, const r3d_instance_t* instances, int count, const render_view_t* view,
                    uint16_t clear) {
    c->frame.predicted_us = 0;
    const int step = current_step(c);
    raster_t* r = &c->raster;
    r->split = &c->split;
    r->instances = instances;
    r->instance_count = count;
    const resolution_step_t scaled = {view->viewport.width * c->scale_percent / 100,
                                      view->viewport.height * c->scale_percent / 100};
    const resolution_step_t size =
        drawn_size(c, step < 0 ? scaled : c->ladder.steps[step], view->viewport.width, view->viewport.height);
    r->width = size.width;
    r->present = view->present;
    r->height = raster_present_size(size, r->present).height;
    r->clear = clear;
    r->upscaled = true; /* render_context_compose() names the picture */
    r->destination_width = view->viewport.width;
    r->destination_height = view->viewport.height;
    if (!fit_scratch(c)) {
        return false;
    }
    c->frame.width = r->width;
    c->frame.height = r->height;
    const int64_t began_us = timing_now_us();
    if (c->policy == RENDER_PREDICTED) {
        /* Culled once: the census prices the frame and the draw reuses its list. */
        c->frame.stats = raster_census(r, view);
        const int chosen = resolution_predict_choose(&c->predict, &c->ladder, c->frame.stats.triangles);
        c->frame.predicted_us = (int32_t)c->predict.chosen_us;
        const resolution_step_t chosen_size =
            drawn_size(c, c->ladder.steps[chosen], r->destination_width, r->destination_height);
        r->width = chosen_size.width;
        r->height = raster_present_size(chosen_size, r->present).height;
        r->band_rows = band_source_rows(r, c->band_rows);
        c->frame.width = r->width;
        c->frame.height = r->height;
        raster_prepare_culled(r, view);
    } else {
        c->frame.stats = raster_prepare(r, view);
    }
    if (r->band_rows == 0) {
        raster_draw_rows(r, 0, r->height);
        show(r);
    }
    /* Fixed: nothing timed or chosen. */
    c->frame.draw_us = c->policy == RENDER_FIXED ? 0 : (int32_t)(timing_now_us() - began_us);
    c->frame.upscale_us = 0;
    c->frame.step = current_step(c);
    return true;
}

/* Composes rows of a `width` by `height` destination, drawing them first
 * when the picture holds a band; the last row feeds the frame's cost. */
static void
compose_rows(render_context_t* c, gfx_target_t destination, int width, int height) {
    raster_t* r = &c->raster;
    const int64_t began_us = timing_now_us();
    if (r->band_rows != 0) {
        int row0;
        int row1;
        raster_upscale_source(r, width, height, destination.y0, destination.y0 + destination.height, &row0, &row1);
        raster_draw_rows(r, row0, row1);
        show(r);
    }
    const int64_t drawn_us = timing_now_us();
    raster_upscale_rows(r, destination, width, height);
    if (c->policy == RENDER_FIXED) {
        return;
    }
    c->frame.draw_us += (int32_t)(drawn_us - began_us);
    c->frame.upscale_us += (int32_t)(timing_now_us() - drawn_us);
    if (destination.y0 + destination.height < height) {
        return;
    }
    const int32_t cost_us = c->frame.draw_us + c->frame.upscale_us;
    if (c->policy == RENDER_STEPPED) {
        (void)resolution_control_update(&c->control, &c->ladder, cost_us);
    } else if (c->policy == RENDER_PREDICTED) {
        resolution_predict_measured(&c->predict, &c->ladder, c->frame.stats.triangles, cost_us);
    }
}

bool
render_context_compose(render_context_t* c, uint16_t* destination, uint16_t* half) {
    const raster_t* r = &c->raster;
    const resolution_step_t size =
        half != NULL ? raster_compose_size(r) : (resolution_step_t){r->destination_width, r->destination_height};
    const bool expanded = half != NULL && size.width != r->destination_width;
    destination = expanded ? half : destination;
    compose_rows(c, (gfx_target_t){destination, 0, size.height, size.width}, size.width, size.height);
    return expanded;
}

void
render_context_compose_rows(render_context_t* c, gfx_target_t destination) {
    compose_rows(c, destination, c->raster.destination_width, c->raster.destination_height);
}

render_context_frame_t
render_context_frame(const render_context_t* c) {
    return c->frame;
}

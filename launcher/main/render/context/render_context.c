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
_Static_assert(RENDER_VIEW_COUNT == sizeof views / sizeof views[0], "view table length");
#endif

const render_debug_view_t*
render_context_view(int view) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    return view > RENDER_VIEW_SHADED && view <= RENDER_VIEW_COUNT ? &views[view - 1] : NULL;
#else
    (void)view;
    return NULL;
#endif
}

int
render_context_view_named(const char* name) {
    if (strcmp(name, "shaded") == 0) {
        return RENDER_VIEW_SHADED;
    }
    for (int i = 1; i <= RENDER_VIEW_COUNT; i++) {
        const render_debug_view_t* row = render_context_view(i);
        if (row != NULL && strcmp(name, row->name) == 0) {
            return i;
        }
    }
    return RENDER_VIEW_UNKNOWN;
}

void
render_context_print_views(FILE* out) {
    (void)fputs("shaded", out);
    for (int i = 1; i <= RENDER_VIEW_COUNT; i++) {
        const render_debug_view_t* row = render_context_view(i);
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
    memory_free(c->view_state);
    *c = (render_context_t){
        .scale_percent = RENDER_CONTEXT_DEFAULT_SCALE_PERCENT,
        .frame = {.step = -1},
    };
}

void
render_context_set_scale(render_context_t* c, int percent) {
    assert(percent > 0 && percent <= 100);
    c->scale_percent = percent;
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
render_context_set_view(render_context_t* c, int view) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    const render_debug_view_t* row = render_context_view(view);
    if (view != RENDER_VIEW_SHADED && row == NULL) {
        return;
    }
    if (c->view == view && (view == RENDER_VIEW_SHADED || c->raster.attachment_count != 0)) {
        return;
    }
    memory_free(c->view_state);
    c->view_state = NULL;
    c->view = RENDER_VIEW_SHADED;
    c->raster.attachment_count = 0;
    c->raster.attachments = NULL;
    if (row == NULL) {
        return;
    }
    if (row->state_bytes != 0) {
        c->view_state = memory_alloc(row->state_bytes, MEMORY_PSRAM);
        if (c->view_state == NULL) {
            return;
        }
        memset(c->view_state, 0, row->state_bytes);
    }
    c->view_attachment = row->attachment(c->view_state);
    c->view_attached[0] = &c->view_attachment;
    c->raster.attachments = c->view_attached;
    c->raster.attachment_count = 1;
    c->view = view;
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

/* Dynamic resolution needs the finest step's scratch so a step never allocates. */
static void
fit_scratch(render_context_t* c) {
    const int width = c->raster.width;
    const int height = c->raster.height;
    if (c->policy != RENDER_FIXED) {
        c->raster.width = c->ladder.steps[0].width;
        c->raster.height = c->ladder.steps[0].height;
    }
    const size_t needed = raster_scratch_bytes(&c->raster);
    c->raster.width = width;
    c->raster.height = height;
    if (needed > c->scratch_bytes) {
        memory_free(c->scratch);
        c->scratch = memory_alloc(needed, MEMORY_PSRAM);
        c->scratch_bytes = c->scratch == NULL ? 0 : needed;
    }
    c->raster.scratch = c->scratch;
}

bool
render_context_draw(render_context_t* c, const r3d_instance_t* instances, int count, const render_view_t* view,
                    uint16_t clear) {
    c->frame.predicted_us = 0;
    const int step = current_step(c);
    raster_t* r = &c->raster;
    r->instances = instances;
    r->instance_count = count;
    r->width = step < 0 ? view->viewport.width * c->scale_percent / 100 : c->ladder.steps[step].width;
    r->height = step < 0 ? view->viewport.height * c->scale_percent / 100 : c->ladder.steps[step].height;
    r->clear = clear;
    r->upscaled = true; /* render_context_compose() names the picture */
    r->destination_width = view->viewport.width;
    r->destination_height = view->viewport.height;
    fit_scratch(c);
    if (c->scratch == NULL) {
        return false;
    }
    c->frame.width = r->width;
    c->frame.height = r->height;
    if (c->policy == RENDER_FIXED) {
        /* Off: the plain draw, nothing timed or chosen. */
        c->frame.stats = raster_draw(r, view);
    } else {
        const int64_t began_us = timing_now_us();
        if (c->policy == RENDER_PREDICTED) {
            /* Culled once: the census prices the frame and the draw reuses its list. */
            c->frame.stats = raster_census(r, view);
            const int chosen = resolution_predict_choose(&c->predict, &c->ladder, c->frame.stats.triangles);
            c->frame.predicted_us = (int32_t)c->predict.chosen_us;
            r->width = c->ladder.steps[chosen].width;
            r->height = c->ladder.steps[chosen].height;
            c->frame.width = r->width;
            c->frame.height = r->height;
            raster_draw_culled(r, view);
        } else {
            c->frame.stats = raster_draw(r, view);
        }
        c->frame.draw_us = (int32_t)(timing_now_us() - began_us);
    }
    c->frame.step = current_step(c);
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (r->attachment_count != 0) {
        raster_show(r);
    }
#endif
    return true;
}

bool
render_context_compose(render_context_t* c, uint16_t* destination, uint16_t* half) {
    const raster_t* r = &c->raster;
    const bool expanded =
        half != NULL && 2 * r->width == r->destination_width && 2 * r->height == r->destination_height;
    destination = expanded ? half : destination;
    const int width = expanded ? r->width : r->destination_width;
    const int height = expanded ? r->height : r->destination_height;
    if (c->policy == RENDER_FIXED) {
        raster_upscale(&c->raster, destination, width, height);
        return expanded;
    }
    const int64_t began_us = timing_now_us();
    raster_upscale(&c->raster, destination, width, height);
    c->frame.upscale_us = (int32_t)(timing_now_us() - began_us);
    const int32_t cost_us = c->frame.draw_us + c->frame.upscale_us;
    if (c->policy == RENDER_STEPPED) {
        (void)resolution_control_update(&c->control, &c->ladder, cost_us);
    } else if (c->policy == RENDER_PREDICTED) {
        resolution_predict_measured(&c->predict, &c->ladder, c->frame.stats.triangles, cost_us);
    }
    return expanded;
}

render_context_frame_t
render_context_frame(const render_context_t* c) {
    return c->frame;
}

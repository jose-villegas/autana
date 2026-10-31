#include "render/context/render_context.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "build/build_variant.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
#include "render/view/raster_boxes.h"
#include "render/view/raster_cull_view.h"
#include "render/view/raster_meshlets.h"
#include "render/view/raster_motion.h"
#include "render/view/raster_show.h"

static const render_debug_view_t views[] = {
    {"depth", 0, raster_depth_view, NULL, NULL},
    {"tiles", 0, raster_depth_tiles_view, NULL, NULL},
    {"motion", sizeof(raster_motion_t), raster_motion_view, NULL, NULL},
    {"meshlets", sizeof(raster_meshlets_t), raster_meshlets_view, NULL, NULL},
    {"cull", sizeof(raster_cull_t), raster_cull_view, raster_cull_second_view, raster_cull_overlay},
    {"boxes", sizeof(raster_boxes_t), raster_boxes_view, NULL, raster_boxes_overlay},
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
render_context_drop_scratch(render_context_t* c) {
    if (c->take == NULL) {
        memory_free(c->scratch);
    }
    c->scratch = NULL;
    c->scratch_bytes = 0;
    c->raster.scratch = NULL;
}

void
render_context_release(render_context_t* c) {
    render_context_drop_scratch(c);
    framegen_target_free(&c->framegen);
    memory_free(c->debug_view_state);
    *c = (render_context_t){
        .scale_percent = RENDER_CONTEXT_DEFAULT_SCALE_PERCENT,
        .frame = {.step = -1},
        .take = c->take,
    };
}

void
render_context_set_take(render_context_t* c, memory_take_fn take) {
    render_context_drop_scratch(c);
    c->take = take;
}

/* Frame generation's measurements, gone: the next pair measures them. */
static void
forget_costs(render_context_t* c) {
    c->generated_us = 0;
    c->drawn_in_row = 0;
    c->paying = false;
}

void
render_context_forget(render_context_t* c) {
    raster_split_forget(&c->split);
    c->kept = false;
    forget_costs(c); /* a new scene or view measures its own */
}

void
render_context_set_frame_generation(render_context_t* c, bool on) {
    /* A caller may set it every frame: only a change forgets the generated cost. */
    if (on == c->generating) {
        return;
    }
    c->generating = on;
    forget_costs(c);
    if (!on) {
        framegen_target_free(&c->framegen);
    }
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
    c->shown_us = config->budget_us;
    c->chose_at_us = 0;
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
        /* A taken block cannot grow or be freed: a larger need takes a new
         * one and abandons the old to its lender. The need only grows, at
         * most to the destination's size, so a lender that gives back per
         * scene loses a few blocks at most. */
        render_context_drop_scratch(c);
        c->scratch = c->take != NULL ? c->take(needed, _Alignof(max_align_t)) : memory_alloc(needed, MEMORY_PSRAM);
        c->scratch_bytes = c->scratch == NULL ? 0 : needed;
    }
    c->raster.scratch = c->scratch;
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
/* The debug view's second draw, when it asks for one, every cluster kept. */
static void
draw_second_view(render_context_t* c) {
    const render_debug_view_t* row = render_context_debug_view(c->debug_view);
    if (row == NULL || row->second_view == NULL) {
        return;
    }
    /* Its own views and clusters: none may steer the frame's core split, and
     * the hook reads render.cull as the frame drew. */
    const tune_entry_t* culling = tune_find(tune_shared(), TUNE_NAME(render, cull));
    const int32_t culled = *culling->value;
    render_view_t second;
    bool listed = false;
    while (row->second_view(c->debug_view_state, &c->raster, &second, &listed)) {
        c->raster.split = NULL;
        if (listed) {
            raster_draw_culled(&c->raster, &second);
        } else {
            (void)tune_set(tune_shared(), TUNE_NAME(render, cull), 0);
            (void)raster_draw(&c->raster, &second);
            (void)tune_set(tune_shared(), TUNE_NAME(render, cull), culled);
        }
        c->raster.split = &c->split;
        listed = false;
    }
}
#endif

/* Whether the last generated frame cost under (100 - FRAMEGEN_GAIN_PERCENT)%
 * of the last drawn one, or while paying already, under all of it. While the
 * drawn size changes the two were drawn at different sizes; the next pair
 * corrects it. */
static bool
generation_pays(const render_context_t* c) {
    const int share = c->paying ? 100 : 100 - FRAMEGEN_GAIN_PERCENT;
    return c->generating && c->generated_us != 0 && (int64_t)c->generated_us * 100 < (int64_t)c->drawn_us * share;
}

/* The ladder the policies price a drawn frame against. While generation
 * pays, a drawn frame and the frames generated from it share their budgets:
 * the drawn one gets what the generated ones leave, never less than one
 * frame's. */
static const resolution_config_t*
policy_config(render_context_t* c) {
    c->ladder.budget_us = c->shown_us;
    if (c->paying) {
        const int32_t shared_us =
            (FRAMEGEN_SHOWN_PER_DRAWN * c->shown_us) - ((FRAMEGEN_SHOWN_PER_DRAWN - 1) * c->generated_us);
        c->ladder.budget_us = shared_us > c->shown_us ? shared_us : c->shown_us;
    }
    return &c->ladder;
}

/* Whether this frame can be warped from the kept one: generation on, a drawn
 * frame kept, no debug view, the same viewport, and a target; and it pays
 * (the verdict the drawn frame is then priced by), has not been measured, or
 * is due to be measured again. */
static bool
can_generate(render_context_t* c, const render_view_t* view) {
    const raster_t* r = &c->raster;
    const viewport_t a = c->kept_view.viewport;
    const viewport_t b = view->viewport;
    c->paying = generation_pays(c);
    const bool worth = c->generated_us == 0 || c->drawn_in_row >= FRAMEGEN_RETRY_DRAWN || c->paying;
    if (!c->generating || !worth || !c->kept || r->attachment_count != 0 || a.width != b.width || a.height != b.height
        || a.quarter != b.quarter) {
        return false;
    }
    if (c->framegen.width != r->width || c->framegen.height != r->height) {
        framegen_target_free(&c->framegen);
        return framegen_target_alloc(&c->framegen, r->width, r->height);
    }
    return true;
}

/* The kept frame warped to `view` into the framegen target, which compose shows. */
static void
generate(render_context_t* c, const render_view_t* view) {
    const int64_t began_us = timing_now_us();
    framegen_warp(&c->raster, &c->kept_view, view, &c->framegen);
    (void)framegen_fill(&c->raster, &c->framegen);
    c->frame.draw_us = (int32_t)(timing_now_us() - began_us);
    c->frame.generated = true;
    c->kept = false; /* FRAMEGEN_SHOWN_PER_DRAWN: the next frame is drawn */
    c->drawn_in_row = 0;
}

bool
render_context_draw(render_context_t* c, const r3d_instance_t* instances, int count, const render_view_t* view,
                    uint16_t clear) {
    if (can_generate(c, view)) {
        generate(c, view);
        return true;
    }
    c->frame.generated = false;
    c->kept = false;
    if (c->drawn_in_row < FRAMEGEN_RETRY_DRAWN) {
        c->drawn_in_row++;
    }
    c->frame.predicted_us = 0;
    const int step = current_step(c);
    raster_t* r = &c->raster;
    r->split = &c->split;
    r->instances = instances;
    r->instance_count = count;
    r->width = step < 0 ? view->viewport.width * c->scale_percent / 100 : c->ladder.steps[step].width;
    r->height = step < 0 ? view->viewport.height * c->scale_percent / 100 : c->ladder.steps[step].height;
    r->present = view->present;
    r->height = raster_present_size((resolution_step_t){r->width, r->height}, r->present).height;
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
    if (c->policy == RENDER_FIXED && !c->generating) {
        /* Off: the plain draw, nothing timed or chosen, unless frame generation needs the drawn cost. */
        c->frame.stats = raster_draw(r, view);
    } else {
        const int64_t began_us = timing_now_us();
        if (c->policy == RENDER_PREDICTED) {
            /* Culled once: the census prices the frame and the draw reuses its list. */
            c->frame.stats = raster_census(r, view);
            const int32_t elapsed_us = c->chose_at_us == 0 ? 0 : (int32_t)(began_us - c->chose_at_us);
            c->chose_at_us = began_us;
            const int chosen =
                resolution_predict_choose(&c->predict, policy_config(c), c->frame.stats.triangles, elapsed_us);
            c->frame.predicted_us = (int32_t)c->predict.chosen_us;
            r->width = c->ladder.steps[chosen].width;
            r->height = raster_present_size(c->ladder.steps[chosen], r->present).height;
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
        draw_second_view(c);
        raster_show(r);
    }
#endif
    c->kept = true;
    c->kept_view = *view;
    return true;
}

void
render_context_set_upscale_filter(render_context_t* c, upscale_filter_t filter) {
    c->raster.upscale.filter = filter;
}

upscale_filter_t
render_context_upscale_filter(const render_context_t* c) {
    return c->raster.upscale.filter;
}

bool
render_context_compose(render_context_t* c, uint16_t* destination, uint16_t* half, uint8_t* half_seams) {
    const raster_t* r = &c->raster;
    const resolution_step_t size =
        half != NULL ? raster_compose_size(r) : (resolution_step_t){r->destination_width, r->destination_height};
    const bool expanded = half != NULL && size.width != r->destination_width;
    destination = expanded ? half : destination;
    uint8_t* seams = expanded ? half_seams : NULL;
    if (c->policy == RENDER_FIXED && !c->generating) {
        raster_upscale_picture(&c->raster, NULL, destination, seams, size.width, size.height);
        return expanded;
    }
    const int64_t began_us = timing_now_us();
    GFX_TARGET_STORAGE(GFX_ATTACHMENT_FURTHER) generated;
    gfx_target_of_color_depth(&generated.target, c->framegen.colour, c->framegen.depth, c->framegen.width,
                              c->framegen.height, 0);
    const gfx_target_t* picture = c->frame.generated ? &generated.target : NULL;
    raster_upscale_picture(&c->raster, picture, destination, seams, size.width, size.height);
    c->frame.upscale_us = (int32_t)(timing_now_us() - began_us);
    if (c->frame.generated) {
        /* Not priced itself: its cost sets what the next drawn frame may spend. */
        c->generated_us = c->frame.draw_us + c->frame.upscale_us;
        return expanded;
    }
    const int32_t cost_us = c->frame.draw_us + c->frame.upscale_us;
    if (c->policy == RENDER_STEPPED) {
        (void)resolution_control_update(&c->control, policy_config(c), cost_us);
    } else if (c->policy == RENDER_PREDICTED) {
        resolution_predict_measured(&c->predict, policy_config(c), c->frame.stats.triangles, cost_us);
    }
    c->drawn_us = cost_us; /* after pricing: this frame was drawn as the last one decided */
    return expanded;
}

render_context_frame_t
render_context_frame(const render_context_t* c) {
    return c->frame;
}

bool
render_context_has_overlay(const render_context_t* c) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    const render_debug_view_t* row = render_context_debug_view(c->debug_view);
    return row != NULL && row->overlay != NULL && c->debug_view_state != NULL;
#else
    (void)c;
    return false;
#endif
}

void
render_context_overlay(const render_context_t* c, int width, int height) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (render_context_has_overlay(c)) {
        render_context_debug_view(c->debug_view)->overlay(c->debug_view_state, width, height);
    }
#else
    (void)c;
    (void)width;
    (void)height;
#endif
}

void*
render_context_debug_view_state(const render_context_t* c) {
    return c->debug_view_state;
}

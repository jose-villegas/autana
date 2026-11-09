#include "render/context/render_context.h"
#include "core/checkpoint.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "build/build_variant.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
#include "render/view/raster_boxes.h"
#include "render/view/raster_copies.h"
#include "render/view/raster_cull_view.h"
#include "render/view/raster_meshlets.h"
#include "render/view/raster_overdraw.h"
#include "render/view/raster_show.h"

static const render_debug_view_t views[] = {
    {.name = "depth", .attachment = raster_depth_view},
    {.name = "tiles", .attachment = raster_depth_tiles_view},
    {.name = "meshlets", .state_bytes = sizeof(raster_meshlets_t), .attachment = raster_meshlets_view},
    {.name = "cull",
     .state_bytes = sizeof(raster_cull_t),
     .attachment = raster_cull_view,
     .second_view = raster_cull_second_view,
     .overlay = raster_cull_overlay},
    {.name = "boxes",
     .state_bytes = sizeof(raster_boxes_t),
     .attachment = raster_boxes_view,
     .overlay = raster_boxes_overlay},
    {.name = "overdraw", .state_bytes = sizeof(raster_overdraw_t), .attachment = raster_overdraw_view},
    {.name = "copies", .state_bytes = sizeof(raster_copies_t), .attachment = raster_copies_view},
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

const char*
render_context_debug_view_name(int view) {
    if (view == RENDER_DEBUG_VIEW_SHADED) {
        return "shaded";
    }
    const render_debug_view_t* row = render_context_debug_view(view);
    return row == NULL ? NULL : row->name;
}

int
render_context_debug_view_named(const char* name) {
    for (int i = RENDER_DEBUG_VIEW_SHADED; i <= RENDER_DEBUG_VIEW_COUNT; i++) {
        const char* row = render_context_debug_view_name(i);
        if (row != NULL && strcmp(name, row) == 0) {
            return i;
        }
    }
    return RENDER_DEBUG_VIEW_UNKNOWN;
}

void
render_context_print_debug_views(FILE* out) {
    (void)fputs(render_context_debug_view_name(RENDER_DEBUG_VIEW_SHADED), out);
    for (int i = RENDER_DEBUG_VIEW_SHADED + 1; i <= RENDER_DEBUG_VIEW_COUNT; i++) {
        const char* name = render_context_debug_view_name(i);
        if (name != NULL) {
            (void)fprintf(out, ", %s", name);
        }
    }
}

static render_context_t main_context = RENDER_CONTEXT_DEFAULT;

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
    render_partial_forget(&c->partial); /* the picture went with it */
}

static void
free_view_state(render_context_t* c) {
    memory_free(c->debug_view_state);
    c->debug_view_state = NULL;
}

void
render_context_release(render_context_t* c) {
    render_context_drop_scratch(c);
    framegen_target_free(&c->framegen);
    free_view_state(c);
    const memory_take_fn take = c->take;
    *c = RENDER_CONTEXT_DEFAULT;
    c->take = take;
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
    render_partial_forget(&c->partial);
    raster_split_forget(&c->split);
    c->raster.pick_round = 0; /* the first frame picks every instance */
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
render_context_set_changed(render_context_t* c, const box3f_t* boxes, int count) {
    render_partial_set_changed(&c->partial, boxes, count);
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
    free_view_state(c);
    c->debug_view = RENDER_DEBUG_VIEW_SHADED;
    c->raster.attachment_count = 0;
    c->raster.attachments = NULL;
    if (row == NULL) {
        return;
    }
    if (row->state_bytes != 0) {
        c->debug_view_state = memory_alloc(row->state_bytes, MEMORY_APP);
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
        c->scratch = c->take != NULL ? c->take(needed, _Alignof(max_align_t)) : memory_alloc(needed, MEMORY_APP);
        c->scratch_bytes = c->scratch == NULL ? 0 : needed;
    }
    c->raster.scratch = c->scratch;
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
/* Where the frame's second view was drawn from. Not inlined: the pose's
 * maths stay off the draw's frame, under the deepest raster chain. */
static __attribute__((noinline)) void
note_observed(render_context_t* c, const transformf_t* observer, const render_view_t* second) {
    c->observed = true;
    c->observed_from = observer != NULL ? *observer : render_view_pose(second);
}

/* The debug view's second draw, when it asks for one, every cluster kept. */
static void
draw_second_view(render_context_t* c) {
    const render_debug_view_t* row = render_context_debug_view(c->debug_view);
    if (row == NULL || row->second_view == NULL) {
        return;
    }
    /* Its own views and clusters: none may steer the frame's core split, and
     * the hook reads the override the frame drew with. */
    const tune_entry_t* culling = tune_find(tune_shared(), TUNE_NAME(render, cull));
    const int32_t culled = *culling->value;
    const transformf_t* observer = c->observing ? &c->observer : NULL;
    render_view_t second;
    bool listed = false;
    while (row->second_view(c->debug_view_state, &c->raster, observer, &second, &listed)) {
        if (!c->observed) {
            note_observed(c, observer, &second);
        }
        c->raster.split = NULL;
        c->raster.picks_levels = false;
        if (listed) {
            raster_draw_culled(&c->raster, &second);
        } else {
            (void)tune_set(tune_shared(), TUNE_NAME(render, cull), 0);
            (void)raster_draw(&c->raster, &second);
            (void)tune_set(tune_shared(), TUNE_NAME(render, cull), culled);
        }
        c->raster.split = &c->split;
        c->raster.picks_levels = true;
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

/* No row changed, but picks are owed (render_partial_picks_owed()): a
 * census makes them, and true when one changed a cut or the depth scale,
 * the census list then drawn over every row. */
static bool
census_changed(render_context_t* c, const render_view_t* view) {
    raster_t* r = &c->raster;
    if (!render_partial_picks_owed(&c->partial, raster_pick_every(r))) {
        return false;
    }
    const float kept_near = raster_depth_near(r);
    c->frame.stats = raster_census(r, view);
    if (!raster_levels_changed(r) && raster_depth_near(r) == kept_near) {
        r->pick_round++; /* the census took this frame's share */
        return false;
    }
    raster_draw_culled(r, view);
    return true;
}

/* The rows partial redraw plans, drawn; false when none changed, so the
 * picture stays as it is. A draw that changed its depth scale or a level
 * pick drew every row. */
static bool
draw_partial(render_context_t* c, const render_view_t* view) {
    raster_t* r = &c->raster;
    const render_rows_t rows = render_partial_rows(&c->partial, r, view);
    render_partial_set_changed(&c->partial, NULL, -1);
    if (rows.first == rows.end) {
        c->frame.unchanged = !census_changed(c, view);
        return !c->frame.unchanged;
    }
    const bool every_row = rows.first == 0 && rows.end == r->height;
    r->first_row = every_row ? 0 : rows.first;
    r->end_row = every_row ? 0 : rows.end;
    c->frame.stats = raster_draw(r, view);
    if (raster_drew_every_row(r)) {
        r->first_row = 0;
        r->end_row = 0;
    }
    return true;
}

/* The frame's bookkeeping as a draw begins: nothing generated, kept or unchanged yet. */
static __attribute__((noinline)) void
begin_draw(render_context_t* c) {
    c->frame.generated = false;
    c->frame.unchanged = false;
    c->kept = false;
    if (c->drawn_in_row < FRAMEGEN_RETRY_DRAWN) {
        c->drawn_in_row++;
    }
    c->frame.predicted_us = 0;
}

/* Aims the raster at this frame's instances, size and destination. */
static __attribute__((noinline)) void
aim_raster(render_context_t* c, const r3d_instance_t* instances, int count, const render_view_t* view, uint16_t clear) {
    const int step = current_step(c);
    raster_t* r = &c->raster;
    r->split = &c->split;
    r->picks_levels = true;
    r->settings = &c->settings;
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
}

/* Off: the plain draw, nothing timed or chosen, unless frame generation needs the drawn cost.
 * False when the partial redraw found nothing to draw. */
static bool
draw_fixed(render_context_t* c, const render_view_t* view) {
    raster_t* r = &c->raster;
    const bool partial = raster_partial_redraw(r);
    if (partial && !draw_partial(c, view)) {
        return false;
    }
    if (!partial) {
        c->frame.stats = raster_draw(r, view);
    }
    return true;
}

/* The draw the resolution policy times, choosing the size first when it predicts. */
static void
draw_timed(render_context_t* c, const render_view_t* view) {
    raster_t* r = &c->raster;
    render_partial_forget(&c->partial);
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

/* Records the drawn frame so the next one can reuse it. Not inlined: a leaf
 * off the draw's deepest chain, keeping its frame out of the draw's. */
static __attribute__((noinline)) void
keep_frame(render_context_t* c, const render_view_t* view) {
    c->kept = true;
    c->kept_view = *view;
    render_partial_kept(&c->partial, &c->raster, view);
}

bool
render_context_draw(render_context_t* c, const r3d_instance_t* instances, int count, const render_view_t* view,
                    uint16_t clear) {
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    c->observed = false;
#endif
    if (can_generate(c, view)) {
        generate(c, view);
        return true;
    }
    begin_draw(c);
    aim_raster(c, instances, count, view, clear);
    fit_scratch(c);
    if (c->scratch == NULL) {
        return false;
    }
    raster_t* r = &c->raster;
    c->frame.width = r->width;
    c->frame.height = r->height;
    r->first_row = 0;
    r->end_row = 0;
    if (c->policy == RENDER_FIXED && !c->generating) {
        if (!draw_fixed(c, view)) {
            return true;
        }
    } else {
        draw_timed(c, view);
    }
    r->pick_round++;
    c->frame.step = current_step(c);
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    if (r->attachment_count != 0) {
        draw_second_view(c);
        raster_show(r);
    }
#endif
    keep_frame(c, view);
    return true;
}

void
render_context_set_settings(render_context_t* c, const render_settings_t* settings) {
    c->settings = *settings;
}

const render_settings_t*
render_context_settings(const render_context_t* c) {
    return &c->settings;
}

void
render_context_set_upscale_filter(render_context_t* c, upscale_filter_t filter) {
    c->raster.upscale.filter = filter;
}

upscale_filter_t
render_context_upscale_filter(const render_context_t* c) {
    return c->raster.upscale.filter;
}

/* The destination rows this compose rewrites, at the view's destination size,
 * from the rows of a compose `height` tall. */
static void
compose_rows(render_context_t* c, int height) {
    const raster_t* r = &c->raster;
    int first = 0;
    int end = height;
    if (c->frame.unchanged) {
        end = 0;
    } else if (r->first_row < r->end_row) {
        raster_destination_rows(r, height, &first, &end);
    }
    const int per = r->destination_height / height;
    c->frame.rows_first = first * per;
    c->frame.rows_end = end * per;
}

bool
render_context_compose(render_context_t* c, uint16_t* destination, uint16_t* half, uint8_t* half_seams) {
    const raster_t* r = &c->raster;
    const resolution_step_t size =
        half != NULL ? raster_compose_size(r) : (resolution_step_t){r->destination_width, r->destination_height};
    const bool expanded = half != NULL && size.width != r->destination_width;
    destination = expanded ? half : destination;
    uint8_t* seams = expanded ? half_seams : NULL;
    if (destination == NULL) {
        (void)render_partial_composed(&c->partial, NULL);
        return false;
    }
    if (!render_partial_composed(&c->partial, destination)) {
        c->frame.unchanged = false;
        c->raster.first_row = 0;
        c->raster.end_row = 0;
    }
    if (c->policy == RENDER_FIXED && !c->generating) {
        compose_rows(c, size.height);
        if (!c->frame.unchanged) {
            raster_upscale_picture(&c->raster, NULL, destination, seams, size.width, size.height);
        }
        return expanded;
    }
    render_partial_forget(&c->partial); /* a generated or resized picture is not the one kept */
    c->frame.rows_first = 0;
    c->frame.rows_end = r->destination_height;
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

int
render_context_rows_first(const render_context_t* c) {
    return c->frame.rows_first;
}

int
render_context_rows_end(const render_context_t* c) {
    return c->frame.rows_end;
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

const void*
render_context_debug_view_state(const render_context_t* c) {
    return c->debug_view_state;
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void
render_context_set_observer(render_context_t* c, const transformf_t* pose) {
    c->observing = pose != NULL;
    c->observer = pose != NULL ? *pose : transformf_identity();
}

bool
render_context_observer_pose(const render_context_t* c, transformf_t* out) {
    if (c->observed) {
        *out = c->observed_from;
    }
    return c->observed;
}

/* The context's bytes a checkpoint holds: all but the observer, which ends it. */
#define CARRIED_BYTES offsetof(render_context_t, observing)
_Static_assert(offsetof(render_context_t, observer) + sizeof(((render_context_t*)0)->observer)
                       + _Alignof(render_context_t)
                   > sizeof(render_context_t),
               "the observer ends render_context_t: a checkpoint holds the bytes before it");

void
render_context_checkpoint_capture(checkpoint_t* saved, render_context_t* context) {
    checkpoint_capture(saved, context, CARRIED_BYTES);
    checkpoint_capture(saved, context->scratch, context->scratch_bytes);
    const render_debug_view_t* row = render_context_debug_view(context->debug_view);
    if (row != NULL) {
        checkpoint_capture_at(saved, &context->debug_view_state, row->state_bytes);
    }
    const size_t bytes = (size_t)context->framegen.width * context->framegen.height * sizeof(uint16_t);
    checkpoint_capture_at(saved, &context->framegen.colour, bytes);
    checkpoint_capture_at(saved, &context->framegen.depth, bytes);
}

static uint16_t*
checkpoint_map(const render_context_t* source, const render_context_t* destination, const uint16_t* map) {
    return map == NULL ? NULL
                       : (uint16_t*)((uint8_t*)destination->scratch + ((uintptr_t)map - (uintptr_t)source->scratch));
}

bool
render_context_checkpoint_prepare(checkpoint_t* saved, render_context_t* context) {
    checkpoint_block_t* block = checkpoint_find(saved, context);
    void* data = block == NULL ? NULL : block->data;
    if (data == NULL) {
        return false;
    }
    render_context_t source = *context;
    memcpy(&source, data, CARRIED_BYTES);
    if (source.take != context->take) {
        return false;
    }
    if (source.scratch_bytes != 0 && (context->scratch == NULL || context->scratch_bytes < source.scratch_bytes)) {
        if (context->take != NULL) {
            return false;
        }
        void* scratch = memory_alloc(source.scratch_bytes, MEMORY_APP);
        if (scratch == NULL) {
            return false;
        }
        memory_free(context->scratch);
        context->scratch = scratch;
        context->scratch_bytes = source.scratch_bytes;
    }
    if (source.debug_view == RENDER_DEBUG_VIEW_SHADED) {
        free_view_state(context);
    }
    source.raster.upscale.columns = checkpoint_map(&source, context, source.raster.upscale.columns);
    source.raster.upscale.rows = checkpoint_map(&source, context, source.raster.upscale.rows);
    source.raster.upscale.blends = checkpoint_map(&source, context, source.raster.upscale.blends);
    (void)checkpoint_retarget(saved, source.scratch, context->scratch);
    source.scratch = context->scratch;
    source.scratch_bytes = context->scratch_bytes;
    source.raster.scratch = context->scratch;
    memcpy(data, &source, CARRIED_BYTES);
    return true;
}
#endif

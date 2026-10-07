#include "render/context/render_context.h"

#include <assert.h>

#include "render/raster_motion.h"
#include "util/build/build_variant.h"
#include "util/runtime/memory.h"
#include "util/runtime/timing.h"

/* raster_show() is linked only where a development build or a host has it. */
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
#define DEBUG_VIEW 1
#else
#define DEBUG_VIEW 0
#endif

#if DEBUG_VIEW
/* Motion vectors, attached while the attachment view shows them, their state
 * in PSRAM; one set, as the main context is the one that shows views. */
static raster_motion_t* motion;
static raster_attachment_t motion_attachment;
static const raster_attachment_t* const motion_attached[] = {&motion_attachment};

static void
attach_motion(render_context_t* c) {
    const bool wanted = c->debug_view >= RASTER_SHOW_ATTACHMENT;
    if (wanted && motion == NULL) {
        motion = memory_alloc(sizeof(*motion), MEMORY_PSRAM);
    }
    if (!wanted || motion == NULL) {
        c->raster.attachment_count = 0;
        return;
    }
    if (c->raster.attachment_count == 0) {
        *motion = (raster_motion_t){0};
        motion_attachment = raster_motion_attachment(motion);
    }
    c->raster.attachments = motion_attached;
    c->raster.attachment_count = 1;
}
#endif

static render_context_t main_context = {
    .scale_percent = RENDER_CONTEXT_DEFAULT_SCALE_PERCENT,
    .debug_view = RASTER_SHOW_SHADED,
    .frame = {.step = -1},
};

render_context_t*
render_context_main(void) {
    return &main_context;
}

void
render_context_release(render_context_t* c) {
    memory_free(c->scratch);
#if DEBUG_VIEW
    memory_free(motion);
    motion = NULL;
#endif
    *c = (render_context_t){
        .scale_percent = RENDER_CONTEXT_DEFAULT_SCALE_PERCENT,
        .debug_view = RASTER_SHOW_SHADED,
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
render_context_set_debug_view(render_context_t* c, raster_show_t mode) {
    c->debug_view = mode;
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
render_context_draw(render_context_t* c, const r3d_instance_t* instances, int count, const camera_t* camera,
                    uint16_t clear, int quarter, int width, int height) {
    c->frame.predicted_us = 0;
    const int step = current_step(c);
    raster_t* r = &c->raster;
    r->instances = instances;
    r->instance_count = count;
    r->width = step < 0 ? width * c->scale_percent / 100 : c->ladder.steps[step].width;
    r->height = step < 0 ? height * c->scale_percent / 100 : c->ladder.steps[step].height;
    r->clear = clear;
    r->upscaled = true; /* render_context_compose() names the picture */
    r->destination_width = width;
    r->destination_height = height;
#if DEBUG_VIEW
    attach_motion(c);
#endif
    fit_scratch(c);
    if (c->scratch == NULL) {
        return false;
    }
    c->frame.width = r->width;
    c->frame.height = r->height;
    if (c->policy == RENDER_FIXED) {
        /* Off: the plain draw, nothing timed or chosen. */
        c->frame.stats = raster_draw(r, camera, quarter);
    } else {
        const int64_t began_us = timing_now_us();
        if (c->policy == RENDER_PREDICTED) {
            /* Culled once: the census prices the frame and the draw reuses its list. */
            c->frame.stats = raster_census(r, camera, quarter);
            const int chosen = resolution_predict_choose(&c->predict, &c->ladder, c->frame.stats.triangles);
            c->frame.predicted_us = (int32_t)c->predict.chosen_us;
            r->width = c->ladder.steps[chosen].width;
            r->height = c->ladder.steps[chosen].height;
            c->frame.width = r->width;
            c->frame.height = r->height;
            raster_draw_culled(r, camera, quarter);
        } else {
            c->frame.stats = raster_draw(r, camera, quarter);
        }
        c->frame.draw_us = (int32_t)(timing_now_us() - began_us);
    }
    c->frame.step = current_step(c);
#if DEBUG_VIEW
    raster_show(r, c->debug_view);
#endif
    return true;
}

void
render_context_compose(render_context_t* c, uint16_t* destination) {
    c->raster.destination = destination;
    if (c->policy == RENDER_FIXED) {
        raster_upscale(&c->raster);
        return;
    }
    const int64_t began_us = timing_now_us();
    raster_upscale(&c->raster);
    c->frame.upscale_us = (int32_t)(timing_now_us() - began_us);
    const int32_t cost_us = c->frame.draw_us + c->frame.upscale_us;
    if (c->policy == RENDER_STEPPED) {
        (void)resolution_control_update(&c->control, &c->ladder, cost_us);
    } else if (c->policy == RENDER_PREDICTED) {
        resolution_predict_measured(&c->predict, &c->ladder, c->frame.stats.triangles, c->frame.draw_us);
    }
}

render_context_frame_t
render_context_frame(const render_context_t* c) {
    return c->frame;
}

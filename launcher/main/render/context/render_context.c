#include "render/context/render_context.h"

#include <assert.h>

#include "util/build/build_variant.h"
#include "util/runtime/memory.h"
#include "util/runtime/timing.h"

/* raster_show() is linked only where a development build or a host has it. */
#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
#define DEBUG_VIEW 1
#else
#define DEBUG_VIEW 0
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

/* Fits the scratch block to what is drawn now (under dynamic resolution,
 * the finest step, so a step never allocates) with the culled list after
 * it; returns the list, or NULL when the block cannot be had. */
static uint16_t*
fit_scratch(render_context_t* c) {
    const int width = c->raster.width;
    const int height = c->raster.height;
    if (c->policy != RENDER_FIXED) {
        c->raster.width = c->ladder.steps[0].width;
        c->raster.height = c->ladder.steps[0].height;
    }
    const size_t raster_bytes = raster_scratch_bytes(&c->raster);
    const size_t needed = raster_bytes + (sizeof(uint16_t) * raster_culled_length(&c->raster));
    c->raster.width = width;
    c->raster.height = height;
    if (needed > c->scratch_bytes) {
        memory_free(c->scratch);
        c->scratch = memory_alloc(needed, MEMORY_PSRAM);
        c->scratch_bytes = c->scratch == NULL ? 0 : needed;
    }
    c->raster.scratch = c->scratch;
    return c->scratch == NULL ? NULL : (uint16_t*)((char*)c->scratch + raster_bytes);
}

bool
render_context_draw(render_context_t* c, const r3d_instance_t* instances, int count, const camera_t* camera,
                    uint16_t clear, int quarter, int width, int height) {
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
    uint16_t* culled = fit_scratch(c);
    if (culled == NULL) {
        return false;
    }
    const int64_t began_us = timing_now_us();
    c->frame.stats = raster_census(r, camera, quarter, culled);
    if (c->policy == RENDER_PREDICTED) {
        const int chosen = resolution_predict_choose(&c->predict, &c->ladder, c->frame.stats.triangles);
        r->width = c->ladder.steps[chosen].width;
        r->height = c->ladder.steps[chosen].height;
    }
    c->frame.step = current_step(c);
    c->frame.width = r->width;
    c->frame.height = r->height;
    raster_draw_culled(r, camera, quarter, culled);
    c->frame.draw_us = (int32_t)(timing_now_us() - began_us);
#if DEBUG_VIEW
    raster_show(r, c->debug_view);
#endif
    return true;
}

void
render_context_compose(render_context_t* c, uint16_t* destination) {
    c->raster.destination = destination;
    const int64_t began_us = timing_now_us();
    raster_upscale(&c->raster);
    c->frame.upscale_us = (int32_t)(timing_now_us() - began_us);
    const int32_t cost_us = c->frame.draw_us + c->frame.upscale_us;
    if (c->policy == RENDER_STEPPED) {
        (void)resolution_control_update(&c->control, &c->ladder, cost_us);
    } else if (c->policy == RENDER_PREDICTED) {
        resolution_predict_measured(&c->predict, &c->ladder, c->frame.stats.triangles, cost_us);
    }
}

render_context_frame_t
render_context_frame(const render_context_t* c) {
    return c->frame;
}

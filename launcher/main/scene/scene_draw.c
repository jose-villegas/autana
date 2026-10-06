/*
 * scene_draw: draws the active scene through its camera with the raster,
 * once per frame.
 */
#include <assert.h>
#include <stdlib.h>

#include "gfx/gfx_color.h"
#include "render/resolution/resolution.h"
#include "scene/scene_internal.h"
#include "util/runtime/memory.h"
#include "util/runtime/timing.h"
#include "util/runtime/tune.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

typedef struct {
    scene_t* scene;
    int camera; /* index into the scene's cameras */
} active_t;

static active_t active;
static bool paused;
static raster_t raster; /* keeps its upscale maps from frame to frame */
static void* scratch;
static size_t scratch_bytes;
static raster_stats_t stats;
static bool stepped;  /* scene_render() ran, and scene_compose() has not yet */
static bool rendered; /* it drew, and scene_compose() has not upscaled it */
#if TUNE_ENABLED
static raster_show_t debug_view = RASTER_SHOW_SHADED;
#endif

/* Dynamic resolution: off, or one of the two policies picking each frame's
 * step. The cost fed back is this frame's draw and upscale. */
static enum { FIXED, STEPPED, PREDICTED } policy;

static resolution_control_t control;
static resolution_predict_t predict;
static scene_resolution_t resolution;

bool
scene_has_active_camera(void) {
    return active.scene != NULL && !paused;
}

void
scene_set_paused(bool pause) {
    paused = pause;
    rendered = false;
    stepped = false;
}

bool
scene_activate(scene_t* scene, const char* camera) {
    const int index = scene_camera_index(scene, camera);
    if (index < 0) {
        return false;
    }
    active = (active_t){scene, index};
    rendered = false;
    return true;
}

void
scene_deactivate(void) {
    active = (active_t){NULL, 0};
    rendered = false;
}

void
scene_draw_forget(const scene_t* scene) {
    if (active.scene == scene) {
        scene_deactivate();
    }
}

void
scene_draw_release(void) {
    scene_deactivate();
    memory_free(scratch);
    scratch = NULL;
    scratch_bytes = 0;
    raster = (raster_t){0};
    stats = (raster_stats_t){0, 0};
    paused = false;
    policy = FIXED;
    resolution = (scene_resolution_t){0};
}

static scene_camera_t*
active_camera(void) {
    return &active.scene->cameras[active.camera];
}

void
scene_set_render_scale(int percent) {
    assert(percent > 0 && percent <= 100);
    if (active.scene != NULL) {
        active_camera()->render_scale_percent = percent;
    }
}

raster_stats_t
scene_stats(void) {
    return stats;
}

void
scene_set_dynamic_resolution(const resolution_config_t* config, const resolution_model_t* model, int first_step) {
    if (config == NULL) {
        policy = FIXED;
        return;
    }
    policy = model == NULL ? STEPPED : PREDICTED;
    resolution_control_init(&control, config, first_step);
    if (model != NULL) {
        resolution_predict_init(&predict, config, model, first_step);
    }
}

scene_resolution_t
scene_resolution(void) {
    return resolution;
}

#if TUNE_ENABLED
void
scene_set_debug_view(raster_show_t mode) {
    debug_view = mode;
}
#endif

/* Rebuilds the placement of each renderer whose entity moved. */
static void
update_placements(scene_t* scene) {
    for (int i = 0; i < scene->asset.renderer_count; i++) {
        scene_renderer_t* r = &scene->renderers[i];
        if ((scene->flags[r->entity] & SCENE_FLAG_DIRTY) != 0) {
            r->placement = scene->transforms[r->entity];
            r->placed = !scene_transform_is_identity(&r->placement);
        }
    }
    for (int i = 0; i < scene->asset.entity_count; i++) {
        scene->flags[i] &= (uint8_t)~SCENE_FLAG_DIRTY;
    }
}

/* The enabled renderers, as the raster draws them. */
static int
fill_instances(scene_t* scene) {
    int count = 0;
    for (int i = 0; i < scene->asset.renderer_count; i++) {
        const scene_renderer_t* r = &scene->renderers[i];
        if ((scene->flags[r->entity] & SCENE_FLAG_ENABLED) != 0) {
            scene->instances[count++] = (r3d_instance_t){&r->mesh, r->placed ? &r->placement : NULL};
        }
    }
    return count;
}

/* Gives the raster a scratch block big enough for what it draws now: under
 * dynamic resolution, for the finest step, so a change never allocates. */
static bool
fit_scratch(void) {
    const int width = raster.width;
    const int height = raster.height;
    if (policy != FIXED) {
        raster.width = control.config.steps[0].width;
        raster.height = control.config.steps[0].height;
    }
    const size_t needed = raster_scratch_bytes(&raster);
    raster.width = width;
    raster.height = height;
    if (needed > scratch_bytes) {
        memory_free(scratch);
        scratch = memory_alloc(needed, MEMORY_PSRAM);
        scratch_bytes = scratch == NULL ? 0 : needed;
    }
    raster.scratch = scratch;
    return scratch != NULL;
}

static void
draw_active(int quarter, int width, int height) {
    scene_t* scene = active.scene;
    const scene_camera_t* camera = active_camera();
    update_placements(scene);
    const int count = fill_instances(scene);
    if (count == 0) {
        return;
    }
    raster.instances = scene->instances;
    raster.instance_count = count;
    const int step = policy == STEPPED ? control.step : predict.step;
    raster.width = policy == FIXED ? width * camera->render_scale_percent / 100 : control.config.steps[step].width;
    raster.height = policy == FIXED ? height * camera->render_scale_percent / 100 : control.config.steps[step].height;
    raster.clear = camera->clear;
    raster.upscaled = true; /* scene_compose() names the picture */
    raster.destination_width = width;
    raster.destination_height = height;
    if (!fit_scratch()) {
        return;
    }
    const camera_t view = r3d_scene_camera_at(&camera->lens, scene->elapsed_ms);
    const int64_t draw_began_us = timing_now_us();
    if (policy == PREDICTED) {
        const resolution_step_t* chosen =
            &predict.config
                 .steps[resolution_predict_choose(&predict, raster_census(&raster, &view, quarter).triangles)];
        raster.width = chosen->width;
        raster.height = chosen->height;
    }
    resolution.step = policy == FIXED ? -1 : (policy == STEPPED ? control.step : predict.step);
    resolution.width = raster.width;
    resolution.height = raster.height;
    stats = raster_draw(&raster, &view, quarter);
    resolution.draw_us = (int32_t)(timing_now_us() - draw_began_us);
#if TUNE_ENABLED
    raster_show(&raster, debug_view);
#endif
    rendered = true;
}

void
scene_render(uint32_t dt_ms, int quarter, int width, int height) {
    if (paused) {
        return;
    }
    stepped = true;
    for (int i = 0; i < scene_loaded_count(); i++) {
        scene_loaded_at(i)->elapsed_ms += dt_ms;
    }
    if (active.scene != NULL) {
        draw_active(quarter, width, height);
    }
}

void
scene_compose(uint32_t dt_ms, int quarter, const scene_target_t* target) {
    if (paused) {
        return;
    }
    if (!stepped) {
        scene_render(dt_ms, quarter, target->width, target->height);
    }
    if (rendered && target->pixels != NULL) {
        raster.destination = target->pixels;
        const int64_t upscale_began_us = timing_now_us();
        raster_upscale(&raster);
        resolution.upscale_us = (int32_t)(timing_now_us() - upscale_began_us);
        const int32_t cost_us = resolution.draw_us + resolution.upscale_us;
        if (policy == STEPPED) {
            (void)resolution_control_update(&control, cost_us);
        } else if (policy == PREDICTED) {
            resolution_predict_measured(&predict, stats.triangles, cost_us);
        }
    }
    rendered = false;
    stepped = false;
}

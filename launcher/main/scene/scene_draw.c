#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "gfx/gfx_color.h"
#include "scene/scene_internal.h"
#include "util/tune.h"

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
    for (int i = 0; i < scene->def->camera_count; i++) {
        if (camera == NULL || strcmp(scene->def->entity_names[scene->cameras[i].entity], camera) == 0) {
            active = (active_t){scene, i};
            rendered = false;
            return true;
        }
    }
    return false;
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
    heap_caps_free(scratch);
    scratch = NULL;
    scratch_bytes = 0;
    raster = (raster_t){0};
    paused = false;
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

void
scene_set_clear(uint32_t rgb) {
    if (active.scene != NULL) {
        active_camera()->clear = GFX_RGB(rgb);
    }
}

raster_stats_t
scene_stats(void) {
    return stats;
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
    for (int i = 0; i < scene->def->renderer_count; i++) {
        scene_renderer_t* r = &scene->renderers[i];
        if ((scene->flags[r->entity] & SCENE_FLAG_DIRTY) != 0) {
            r->placement = scene->transforms[r->entity];
            r->placed = !scene_transform_is_identity(&r->placement);
        }
    }
    for (int i = 0; i < scene->def->entity_count; i++) {
        scene->flags[i] &= (uint8_t)~SCENE_FLAG_DIRTY;
    }
}

/* The enabled renderers, as the raster draws them. */
static int
fill_instances(scene_t* scene) {
    int count = 0;
    for (int i = 0; i < scene->def->renderer_count; i++) {
        const scene_renderer_t* r = &scene->renderers[i];
        if ((scene->flags[r->entity] & SCENE_FLAG_ENABLED) != 0) {
            scene->instances[count++] = (r3d_instance_t){&r->mesh, r->placed ? &r->placement : NULL};
        }
    }
    return count;
}

/* Gives the raster a scratch block big enough for what it draws now. */
static bool
fit_scratch(void) {
    const size_t needed = raster_scratch_bytes(&raster);
    if (needed > scratch_bytes) {
        heap_caps_free(scratch);
        scratch = heap_caps_malloc(needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        scratch_bytes = scratch == NULL ? 0 : needed;
    }
    raster.scratch = scratch;
    return scratch != NULL;
}

static void
draw_active(int quarter, const scene_target_t* target) {
    scene_t* scene = active.scene;
    const scene_camera_t* camera = active_camera();
    update_placements(scene);
    const int count = fill_instances(scene);
    if (count == 0) {
        return;
    }
    raster.instances = scene->instances;
    raster.instance_count = count;
    raster.width = target->width * camera->render_scale_percent / 100;
    raster.height = target->height * camera->render_scale_percent / 100;
    raster.clear = camera->clear;
    raster.destination = target->pixels;
    raster.destination_width = target->width;
    raster.destination_height = target->height;
    if (!fit_scratch()) {
        return;
    }
    const camera_t view = r3d_scene_camera_at(&camera->lens, scene->elapsed_ms);
    stats = raster_draw(&raster, &view, quarter);
#if TUNE_ENABLED
    raster_show(&raster, debug_view);
#endif
    rendered = true;
}

void
scene_render(uint32_t dt_ms, int quarter, const scene_target_t* target) {
    if (paused) {
        return;
    }
    stepped = true;
    for (int i = 0; i < scene_loaded_count(); i++) {
        scene_loaded_at(i)->elapsed_ms += dt_ms;
    }
    if (active.scene != NULL && target->pixels != NULL) {
        draw_active(quarter, target);
    }
}

void
scene_compose(uint32_t dt_ms, int quarter, const scene_target_t* target) {
    if (paused) {
        return;
    }
    if (!stepped) {
        scene_render(dt_ms, quarter, target);
    } else if (!rendered && active.scene != NULL && target->pixels != NULL) {
        draw_active(quarter, target); /* scene_render() had no framebuffer to name yet */
    }
    if (rendered) {
        raster_upscale(&raster);
    }
    rendered = false;
    stepped = false;
}

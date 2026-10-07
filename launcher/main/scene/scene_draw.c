/*
 * scene_draw: draws the active scene through its camera with the raster,
 * once per frame.
 */
#include <assert.h>
#include <stdlib.h>

#include "gfx/gfx_color.h"
#include "render/raster_motion.h"
#include "scene/scene_internal.h"
#include "util/runtime/memory.h"
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
/* Motion vectors, attached while the attachment view shows them. */
static raster_motion_t* motion;
static raster_attachment_t motion_attachment;
static const raster_attachment_t* const attachments[] = {&motion_attachment};
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
    const int index = scene_camera_index(scene, camera);
    if (index < 0) {
        return false;
    }
    active = (active_t){scene, index};
    rendered = false;
#if TUNE_ENABLED
    if (motion != NULL) {
        raster_motion_forget(motion); /* a cut */
    }
#endif
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
#if TUNE_ENABLED
    memory_free(motion);
    motion = NULL;
#endif
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

#if TUNE_ENABLED
void
scene_set_debug_view(raster_show_t mode) {
    debug_view = mode;
}

/* Attaches motion while it is shown, its state in PSRAM; a picture after a
 * gap has no previous one. */
static void
attach_motion(void) {
    const bool wanted = debug_view >= RASTER_SHOW_ATTACHMENT;
    if (wanted && motion == NULL) {
        motion = memory_alloc(sizeof(*motion), MEMORY_PSRAM);
    }
    if (!wanted || motion == NULL) {
        raster.attachment_count = 0;
        return;
    }
    if (raster.attachment_count == 0) {
        *motion = (raster_motion_t){0};
        motion_attachment = raster_motion_attachment(motion);
    }
    raster.attachments = attachments;
    raster.attachment_count = 1;
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

/* Gives the raster a scratch block big enough for what it draws now. */
static bool
fit_scratch(void) {
    const size_t needed = raster_scratch_bytes(&raster);
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
    raster.width = width * camera->render_scale_percent / 100;
    raster.height = height * camera->render_scale_percent / 100;
    raster.clear = camera->clear;
    raster.upscaled = true; /* scene_compose() names the picture */
    raster.destination_width = width;
    raster.destination_height = height;
#if TUNE_ENABLED
    attach_motion();
#endif
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
        raster_upscale(&raster);
    }
    rendered = false;
    stepped = false;
}

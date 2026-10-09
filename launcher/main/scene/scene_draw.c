/*
 * scene_draw: draws the active scene through its camera, once per frame,
 * into the engine's render context, which decides the size and quality.
 */
#include <assert.h>
#include <stdlib.h>

#include "render/context/render_context.h"
#include "scene/scene_internal.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

typedef struct {
    scene_t* scene;
    int camera; /* index into the scene's cameras */
} active_t;

static active_t active;
static bool paused;
static bool expanded;
static bool stepped;  /* scene_render() ran, and scene_compose() has not yet */
static bool rendered; /* it drew, and scene_compose() has not upscaled it */

bool
scene_has_active_camera(void) {
    return active.scene != NULL && (!paused || expanded);
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
    stepped = false;
    expanded = false;
    return true;
}

void
scene_deactivate(void) {
    active = (active_t){NULL, 0};
    rendered = false;
    stepped = false;
    expanded = false;
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
    render_context_release(render_context_main());
    paused = false;
}

static scene_camera_t*
active_camera(void) {
    return &active.scene->cameras[active.camera];
}

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

static void
draw_active(int quarter, int width, int height) {
    scene_t* scene = active.scene;
    const scene_camera_t* camera = active_camera();
    update_placements(scene);
    const int count = fill_instances(scene);
    if (count == 0) {
        return;
    }
    const render_view_t view =
        r3d_scene_view_at(&camera->lens, scene->elapsed_ms, (viewport_t){width, height, quarter});
    rendered = render_context_draw(render_context_main(), scene->instances, count, &view, camera->clear);
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

bool
scene_compose(uint32_t dt_ms, int quarter, const scene_target_t* target) {
    if (target->half_pixels == NULL) {
        expanded = false;
    }
    if (paused) {
        return expanded;
    }
    if (!stepped) {
        scene_render(dt_ms, quarter, target->width, target->height);
    }
    if (rendered && target->pixels != NULL) {
        expanded = render_context_compose(render_context_main(), target->pixels, target->half_pixels);
    }
    rendered = false;
    stepped = false;
    return expanded;
}

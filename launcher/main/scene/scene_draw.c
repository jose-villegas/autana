/*
 * scene_draw: draws the active scene through its camera, once per frame,
 * into the engine's render context, which decides the size and quality.
 */
#include <assert.h>
#include <stdlib.h>

#include "esp_log.h"

#include "render/context/render_context.h"
#include "scene/camera/camera.h"
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
static bool refused;  /* the context had no scratch for the active camera, logged once */

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

void
scene_draw_activate(scene_t* scene, int camera) {
    active = (active_t){scene, camera};
    render_context_forget(render_context_main());
    refused = false;
    rendered = false;
    stepped = false;
    expanded = false;
}

void
camera_deactivate(void) {
    active = (active_t){NULL, 0};
    rendered = false;
    stepped = false;
    expanded = false;
}

void
scene_draw_forget(const scene_t* scene) {
    if (active.scene == scene) {
        camera_deactivate();
    }
}

void
scene_draw_release(void) {
    camera_deactivate();
    render_context_release(render_context_main());
    paused = false;
}

void
scene_draw_camera_cut(const scene_t* scene, scene_row_t entity) {
    if (active.scene == scene && scene->cameras[active.camera].entity == entity) {
        render_context_forget(render_context_main());
    }
}

static scene_camera_t*
active_camera(void) {
    return &active.scene->cameras[active.camera];
}

/* The active renderers with something to draw, as the raster draws them;
 * scene_resolve() has listed them. */
static int
fill_instances(scene_t* scene) {
    int count = 0;
    for (int i = 0; i < scene->active_renderer_count; i++) {
        const scene_renderer_t* r = &scene->renderers[scene->active_renderers[i]];
        if (r->mesh.node_count > 0) {
            scene->instances[count++] = (r3d_instance_t){&r->mesh, r->placed ? &r->model : NULL, &r->world_bounds};
        }
    }
    return count;
}

/* The active camera's view from its entity's world. Its own frame, gone
 * before the draw: the pose and the world walk stay off the draw's stack. */
static __attribute__((noinline)) render_view_t
active_view(const scene_t* scene, const scene_camera_t* camera, viewport_t viewport) {
    const transformf_t pose = scene_row_world(scene->root, camera->entity);
    return lens_view(&camera->lens, &pose, viewport);
}

static void
draw_active(int quarter, int width, int height) {
    scene_t* scene = active.scene;
    const scene_camera_t* camera = active_camera();
    scene_resolve(scene);
    const int count = fill_instances(scene);
    if (count == 0) {
        return;
    }
    const render_view_t view = active_view(scene, camera, (viewport_t){width, height, quarter});
    rendered = render_context_draw(render_context_main(), scene->instances, count, &view, camera->clear);
    if (!rendered && !refused) {
        ESP_LOGE("scene", "no memory to draw '%s'; it stays blank", scene->root->id);
        refused = true;
    }
}

void
scene_render(uint32_t dt_ms, int quarter, int width, int height) {
    if (paused) {
        return;
    }
    stepped = true;
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        scene_t* scene = scene_ready_at(i);
        if (scene != NULL) {
            scene_animation_step(scene, dt_ms);
        }
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
        expanded =
            render_context_compose(render_context_main(), target->pixels, target->half_pixels, target->half_seams);
    }
    rendered = false;
    stepped = false;
    return expanded;
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void
scene_draw_checkpoint_capture(checkpoint_t* saved) {
    CHECKPOINT_CAPTURE(saved, active);
    CHECKPOINT_CAPTURE(saved, paused);
    CHECKPOINT_CAPTURE(saved, expanded);
    CHECKPOINT_CAPTURE(saved, stepped);
    CHECKPOINT_CAPTURE(saved, rendered);
    CHECKPOINT_CAPTURE(saved, refused);
    scene_world_checkpoint_capture(saved);
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        scene_t* scene = scene_ready_at(i);
        if (scene != NULL) {
            scene_checkpoint_capture(saved, scene);
        }
    }
}
#endif

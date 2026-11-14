/*
 * scene_draw.c stand-ins for host tools that run the scene without drawing: with no camera_activate() no scene is
 * active, where scene_draw.c's versions do nothing too; linking scene_draw.c needs the whole renderer source
 * list. Any drawing call still fails to link. scene_host.py lists this beside the scene sources.
 * The render context scene.c lends its scratch to is stood in for the same reason: without drawing there is no
 * context, so scene.c's calls on it have nothing to set or drop.
 */
#include <stdlib.h>

#include "render/context/render_context.h"
#include "render/r3d_pipeline.h"
#include "scene/scene_internal.h"

render_context_t*
render_context_main(void) {
    return NULL;
}

void
render_context_set_take(render_context_t* context, memory_take_fn take) {
    (void)context;
    (void)take;
}

void
render_context_drop_scratch(render_context_t* context) {
    (void)context;
}

/* No context, so the animation tiers read the shipped defaults. */
const render_settings_t*
render_context_settings(const render_context_t* context) {
    (void)context;
    static const render_settings_t defaults = RENDER_SETTINGS_DEFAULT;
    return &defaults;
}

void
scene_draw_forget(const scene_t* scene) {
    (void)scene;
}

void
scene_draw_release(void) {}

void
scene_draw_camera_cut(const scene_t* scene, scene_row_t entity) {
    (void)scene;
    (void)entity;
}

void
scene_draw_activate(scene_t* scene, int camera) {
    (void)scene;
    (void)camera;
}

void
camera_deactivate(void) {}

/* Called only by r3d_batch.c's drawing half, linked for the bounds the scene reads; nothing draws here. */
void
r3d_lens_place(r3d_lens_t* lens, const mat4f_t* model, int position_scale) {
    (void)lens;
    (void)model;
    (void)position_scale;
    abort();
}

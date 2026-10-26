/*
 * scene_draw.c stand-ins for host tools that run the scene without drawing: with no scene_activate() no scene is
 * active, where scene_draw.c's versions do nothing too; linking scene_draw.c needs the whole renderer source
 * list. Any drawing call still fails to link. scene_host.py lists this beside the scene sources.
 * The render context scene.c lends its scratch to is stood in for the same reason: without drawing there is no
 * context, so scene.c's calls on it have nothing to set or drop.
 */
#include "render/context/render_context.h"
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

void
scene_draw_forget(const scene_t* scene) {
    (void)scene;
}

void
scene_draw_release(void) {}

void
scene_draw_camera_cut(const scene_t* scene, scene_entity_t entity) {
    (void)scene;
    (void)entity;
}

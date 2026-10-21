/*
 * scene_draw.c stand-ins for host tools that run the scene without drawing: with no scene_activate() no scene is
 * active, where scene_draw.c's versions do nothing too; linking scene_draw.c needs the whole renderer source
 * list. Any drawing call still fails to link. scene_host.py lists this beside the scene sources.
 */
#include "scene/scene_internal.h"

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

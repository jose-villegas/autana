/* Animator: find an entity at load and blend to a selected clip. */
#include "scene/scene.h"

#define ANIMATOR_EXAMPLE_BLEND_MS 250

static scene_t* animated_scene;
static scene_entity_t subject;

static void
animator_example_start(const char* scene_id, const char* entity_name) {
    animated_scene = scene_load(scene_id, NULL);
    subject = scene_find(animated_scene, entity_name);
    scene_activate(animated_scene, NULL);
}

static bool
animator_example(int clip) {
    return scene_animator_play(animated_scene, subject, clip, ANIMATOR_EXAMPLE_BLEND_MS);
}

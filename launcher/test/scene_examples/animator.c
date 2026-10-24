/* Animator: find an entity at load and blend to a selected clip. */
#include "scene/scene.h"

#define BLEND_MS 250

static scene_t* scene;
static scene_entity_t subject;

static void
start(void) {
    scene = scene_load("meadow", NULL);
    subject = scene_find(scene, "subject");
    scene_activate(scene, NULL);
}

static bool
on_pick(int clip) {
    return scene_animator_play(scene, subject, clip, BLEND_MS);
}

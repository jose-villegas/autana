/* Animator: find an entity at load and blend to a selected clip. */
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/scene.h"

#define BLEND_MS 250

static entity_t scene;
static entity_t subject;

static void
start(void) {
    scene = scene_load("meadow");
    subject = entity_find(scene, name_hash("subject"));
    camera_activate(camera_first(scene));
}

static bool
on_pick(int clip) {
    return animator_play(subject, clip, BLEND_MS);
}

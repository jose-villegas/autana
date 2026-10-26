/* Small fill: choose the fast policy for a scene camera. */
#include "scene/scene.h"

static bool
small_fill_example(scene_t* scene, scene_entity_t camera) {
    return scene_camera_set_small_fill(scene, camera, SMALL_FILL_FAST);
}

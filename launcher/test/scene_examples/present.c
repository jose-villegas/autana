/* Presentation: choose how the camera's rendered rows reach the panel. */
#include "scene/scene.h"

static bool
present_example(scene_t* scene, scene_entity_t camera, camera_present_t mode) {
    return scene_camera_set_present(scene, camera, mode);
}

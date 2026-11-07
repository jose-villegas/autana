/* Presentation: choose how the camera's rendered rows reach the panel. */
#include "scene/camera/camera.h"
#include "scene/scene.h"

static bool
present_example(entity_t cam) {
    return camera_set_present(cam, CAMERA_PRESENT_SCANLINE);
}

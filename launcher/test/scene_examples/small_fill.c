/* Small fill: choose the fast policy for a scene camera. */
#include "scene/camera/camera.h"
#include "scene/scene.h"

static bool
small_fill_example(entity_t camera) {
    return camera_set_small_fill(camera, SMALL_FILL_FAST);
}

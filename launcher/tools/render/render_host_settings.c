/* The renderer's halves of render_host.c: --set render.FIELD=value reaches
 * the main render context as an app's pack sets it, and --poses reads the
 * active scene camera. */
#include "render_host.h"

#include "scene/camera/camera.h"
#include "scene/transform/transform.h"

bool
render_host_apply_settings(const render_settings_t* settings) {
    render_context_set_settings(render_context_main(), settings);
    return true;
}

bool
render_host_camera_pose(float position[3], float rotation[4]) {
    const entity_t camera = camera_active();
    if (handle_is_none(camera)) {
        return false;
    }
    const transformf_t world = transform_world(camera);
    position[0] = world.position.x;
    position[1] = world.position.y;
    position[2] = world.position.z;
    rotation[0] = world.rotation.x;
    rotation[1] = world.rotation.y;
    rotation[2] = world.rotation.z;
    rotation[3] = world.rotation.w;
    return true;
}

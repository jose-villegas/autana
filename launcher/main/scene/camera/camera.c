#include "scene/camera/camera.h"

#include <stddef.h>

render_view_t
camera_view(const camera_t* camera, const transformf_t* pose, viewport_t viewport) {
    return render_view_make(pose, camera->half_fov_short_tan, camera->near_z, viewport);
}

static const anim_field_t LENS_FIELDS[] = {
    ANIM_FIELD(camera_t, half_fov_short_tan, ANIM_VALUE_FLOAT),
    ANIM_FIELD(camera_t, near_z, ANIM_VALUE_FLOAT),
};
const anim_component_fields_t CAMERA_FIELDS = {
    ANIM_COMPONENT_CAMERA,
    LENS_FIELDS,
    sizeof LENS_FIELDS / sizeof LENS_FIELDS[0],
};

#include "scene/camera/camera.h"

#include <stddef.h>

render_view_t
camera_view(const camera_t* camera, const transformf_t* pose, viewport_t viewport) {
    render_view_t view = render_view_make(pose, camera->half_fov_short_tan, camera->near_z, viewport);
    view.present = camera->present;
    view.small_fill = camera->small_fill;
    view.skip_hidden = camera->skip_hidden;
    if (camera->shift.x != 0.0F || camera->shift.y != 0.0F) {
        render_view_set_shift(&view, camera->shift);
    }
    return view;
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

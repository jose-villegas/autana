#include "scene/camera/lens.h"

#include <stddef.h>

render_view_t
lens_view(const lens_t* camera, const transformf_t* pose, viewport_t viewport) {
    render_view_t view = render_view_make(pose, camera->half_fov_short_tan, camera->near_z, viewport);
    view.present = camera->present;
    view.small_fill = camera->small_fill;
    view.skip_hidden = camera->skip_hidden;
    if (camera->shift.x != 0.0F || camera->shift.y != 0.0F) {
        render_view_set_shift(&view, camera->shift);
    }
    return view;
}

static const anim_field_t LENS_FIELD_LIST[] = {
    ANIM_FIELD(lens_t, half_fov_short_tan, ANIM_VALUE_FLOAT),
    ANIM_FIELD(lens_t, near_z, ANIM_VALUE_FLOAT),
};
const anim_component_fields_t LENS_FIELDS = {
    ANIM_COMPONENT_CAMERA,
    LENS_FIELD_LIST,
    sizeof LENS_FIELD_LIST / sizeof LENS_FIELD_LIST[0],
};

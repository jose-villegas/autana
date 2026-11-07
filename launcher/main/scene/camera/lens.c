#include "scene/camera/lens.h"

#include <stddef.h>

render_view_t
lens_view(const lens_t* lens, const transformf_t* pose, viewport_t viewport) {
    render_view_t view = render_view_make(pose, lens->half_fov_short_tan, lens->near_z, viewport);
    view.present = lens->present;
    view.small_fill = lens->small_fill;
    view.skip_hidden = lens->skip_hidden;
    if (lens->shift.x != 0.0F || lens->shift.y != 0.0F) {
        render_view_set_shift(&view, lens->shift);
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

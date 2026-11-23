#include "scene/hinge/hinge.h"

static const anim_field_t FIELDS[] = {
    ANIM_FIELD(scene_hinge_t, angle, ANIM_VALUE_FLOAT),
};

const anim_component_fields_t HINGE_FIELDS = {ANIM_COMPONENT_HINGE, FIELDS, HINGE_FIELD_COUNT};

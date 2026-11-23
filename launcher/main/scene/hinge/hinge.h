/* hinge: a scene joint driven by its authored angle or a timeline field. */
#pragma once

#include "anim/anim_binding.h"
#include "kinematics/hinge/hinge.h"

#define ANIM_COMPONENT_HINGE ASSET_TYPE('H', 'N', 'G', 'E')

enum { HINGE_FIELD_COUNT = 1 };

extern const anim_component_fields_t HINGE_FIELDS;

typedef struct {
    hinge_t joint;
    float angle;
} scene_hinge_t;

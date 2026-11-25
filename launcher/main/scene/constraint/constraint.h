/* Constraints blend a scene object's position and rotation toward weighted inputs. */
#pragma once

#include "anim/anim_binding.h"
#include "scene/scene_format.h"

#define ANIM_COMPONENT_POSITION_CONSTRAINT ASSET_TYPE('C', 'P', 'O', 'S')
#define ANIM_COMPONENT_AIM_CONSTRAINT      ASSET_TYPE('C', 'A', 'I', 'M')

#define CONSTRAINT_WEIGHT_FIELD_LIST                                                                                   \
    ANIM_FIELD(scene_constraint_weights_t, weight, ANIM_VALUE_FLOAT),                                                  \
        ANIM_FIELD(scene_constraint_weights_t, weight0, ANIM_VALUE_FLOAT),                                             \
        ANIM_FIELD(scene_constraint_weights_t, weight1, ANIM_VALUE_FLOAT),                                             \
        ANIM_FIELD(scene_constraint_weights_t, weight2, ANIM_VALUE_FLOAT),                                             \
        ANIM_FIELD(scene_constraint_weights_t, weight3, ANIM_VALUE_FLOAT)

enum {
    CONSTRAINT_WEIGHT_FIELD,
    CONSTRAINT_FIRST_SOURCE_FIELD,
    CONSTRAINT_WEIGHT_FIELD_COUNT = CONSTRAINT_FIRST_SOURCE_FIELD + SCENE_CONSTRAINT_SOURCES_MAX
};

extern const anim_component_fields_t POSITION_CONSTRAINT_FIELDS;
extern const anim_component_fields_t AIM_CONSTRAINT_FIELDS;

/* constraint: timeline bindings for scene follow and look-at. */
#pragma once

#include "anim/anim_binding.h"
#include "anim/anim_tracks_format.h"

#define ANIM_COMPONENT_CONSTRAINT ASSET_TYPE('C', 'N', 'S', 'T')

enum {
    CONSTRAINT_FIELD_FOLLOW,
    CONSTRAINT_FIELD_LOOK_AT,
    CONSTRAINT_FIELD_LAG_MS,
    CONSTRAINT_FIELD_HANDOFF_MS,
    CONSTRAINT_FIELD_OFFSET,
    CONSTRAINT_FIELD_LOOK_OFFSET,
    CONSTRAINT_FIELD_WEIGHT,
    CONSTRAINT_FIELD_COUNT,
    CONSTRAINT_TARGET_COUNT = CONSTRAINT_FIELD_LAG_MS,
    CONSTRAINT_STEP_COUNT = CONSTRAINT_FIELD_OFFSET,
};

extern const anim_component_fields_t CONSTRAINT_FIELDS;

static inline bool
scene_constraint_duration_fits(float ms) {
    return ms >= 0.0F && ms < (float)ANIM_TRACKS_DURATION_MAX;
}

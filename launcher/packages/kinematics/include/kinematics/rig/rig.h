/* rig: stateless camera follow positions and look rotations. */
#pragma once

#include "math/linear/transformf.h"

typedef enum { RIG_BIND_WORLD, RIG_BIND_TARGET, RIG_BIND_TARGET_YAW } rig_bind_t;

/* Follow uses world axes, full target rotation, or the horizontal target heading.
 * An almost vertical target forward uses world axes. Target scale has no effect. */
vec3f_t rig_follow_position(const transformf_t* target, vec3f_t offset, rig_bind_t bind);

/* Aims local +Z with the roll and pole fallback of quatf_looking. */
quatf_t rig_look_rotation(vec3f_t from, vec3f_t at, vec3f_t up);

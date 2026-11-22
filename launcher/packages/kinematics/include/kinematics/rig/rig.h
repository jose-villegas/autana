/* rig: follow positions, look rotations and hand-off weights sampled on a timeline. */
#pragma once

#include <stdint.h>

#include "math/linear/transformf.h"

typedef enum { RIG_BIND_WORLD, RIG_BIND_TARGET, RIG_BIND_TARGET_YAW } rig_bind_t;

/* Offset from a target sampled at t - lag. RIG_BIND_WORLD uses world axes,
 * RIG_BIND_TARGET uses full rotation, and RIG_BIND_TARGET_YAW uses local -Z
 * projected onto the ground plane. A vertical heading within float roundoff
 * uses world axes. Target scale is ignored. */
vec3f_t rig_follow_position(transformf_t target, vec3f_t offset, rig_bind_t bind);

/* Local -Z aims from the first point to the second, with zero roll about up. Coincident points or a direction
 * parallel to up return keep. */
quatf_t rig_look_rotation(vec3f_t from, vec3f_t at, vec3f_t up, quatf_t keep);

/* Smoothstep from 0 to 1 over blend_ms, or 1 immediately if blend_ms is zero. */
float rig_handoff(uint32_t since_ms, uint32_t blend_ms);

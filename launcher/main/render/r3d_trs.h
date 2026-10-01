/*
 * r3d_trs: an animation track's sampled translation, rotation and scale as a
 * transform_t, for a caller that plays a track (anim/) onto an object.
 *
 * Translation and scale are in model units; the rotation is a quaternion
 * (xyzw), renormalized because a sample between two keys is a lerp.
 * Header-only, static inline and ESP-IDF-free.
 */
#pragma once

#include "util/math/transform.h"

static inline transform_t
r3d_trs_to_transform(const float translation[3], const float rotation[4], const float scale[3]) {
    transform_t t = TRANSFORM_IDENTITY;
    transform_set_position(&t, (vec3_t){translation[0], translation[1], translation[2]});
    transform_set_rotation(&t, quat_normalize((quat_t){rotation[0], rotation[1], rotation[2], rotation[3]}));
    transform_set_scale(&t, (vec3_t){scale[0], scale[1], scale[2]});
    return t;
}

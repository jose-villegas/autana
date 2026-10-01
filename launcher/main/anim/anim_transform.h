/*
 * anim_transform: a node's translation, rotation and scale tracks sampled
 * into a transformf_t, for an object an animation plays onto.
 *
 * Translation and scale are in model units; the rotation track is a
 * quaternion (xyzw), renormalized because a sample between two keys is a
 * lerp. Header-only, static inline and ESP-IDF-free.
 */
#pragma once

#include "anim/anim_track.h"
#include "util/math/transformf.h"

static inline transformf_t
anim_transform_sample(const anim_track_t* move, const anim_track_t* turn, const anim_track_t* size, float seconds) {
    float t[ANIM_WIDTH_MAX];
    float q[ANIM_WIDTH_MAX];
    float s[ANIM_WIDTH_MAX];
    anim_track_sample(move, seconds, t);
    anim_track_sample(turn, seconds, q);
    anim_track_sample(size, seconds, s);

    transformf_t node = TRANSFORMF_IDENTITY;
    transformf_set_position(&node, (vec3f_t){t[0], t[1], t[2]});
    transformf_set_rotation(&node, quatf_normalize((quatf_t){q[0], q[1], q[2], q[3]}));
    transformf_set_scale(&node, (vec3f_t){s[0], s[1], s[2]});
    return node;
}

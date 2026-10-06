/*
 * anim_transform: a node's translation, rotation and scale tracks sampled
 * into a transformf_t, for an object an animation plays onto.
 *
 * Translation and scale are in model units; the rotation track is a
 * quaternion (xyzw). Samples between keys are already unit (slerp or a
 * normalized Hermite); the normalize guards a held key that was authored a
 * hair off unit length. Header-only, static inline and ESP-IDF-free.
 */
#pragma once

#include "anim/anim_track.h"
#include "util/math/transformf.h"

static inline transformf_t
anim_transform_sample(const anim_node_tracks_t* node, float seconds) {
    float t[ANIM_WIDTH_MAX];
    float q[ANIM_WIDTH_MAX];
    float s[ANIM_WIDTH_MAX];
    anim_track_sample(&node->translation, seconds, t);
    anim_track_sample(&node->rotation, seconds, q);
    anim_track_sample(&node->scale, seconds, s);

    transformf_t pose = TRANSFORMF_IDENTITY;
    transformf_set_position(&pose, (vec3f_t){t[0], t[1], t[2]});
    transformf_set_rotation(&pose, quatf_normalize((quatf_t){q[0], q[1], q[2], q[3]}));
    transformf_set_scale(&pose, (vec3f_t){s[0], s[1], s[2]});
    return pose;
}

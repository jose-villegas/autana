/*
 * r3d_trs: float translation, rotation and scale as one fix3
 * transform, for a caller that plays an animation track (anim/) onto an
 * object drawn in fixed point.
 *
 * Translation and scale are in model units, one to FIX3_ONE. The rotation is a
 * unit quaternion (xyzw) acting on column vectors in fix3's own frame,
 * and comes back as the Euler angles fix3 composes: Z, then X, then Y,
 * in turns. Angles land on whole fixed-point units, so a rotation sampled
 * between two keys moves in steps of one; the same rotation reached another
 * way round (x and z either side of a half turn) is an equal matrix.
 */
#pragma once

#include "render/fix3.h"

fix3_transform_t r3d_trs_to_transform(const float translation[3], const float rotation[4], const float scale[3]);

/*
 * r3d_trs: float translation, rotation and scale as one mat4i
 * transform, for a caller that plays an animation track (anim/) onto an
 * object drawn in fixed point.
 *
 * Translation and scale are in model units, one to M4_ONE. The rotation is a
 * unit quaternion (xyzw) acting on column vectors in mat4i's own frame,
 * and comes back as the Euler angles mat4i composes: Z, then X, then Y,
 * in turns. Angles land on whole fixed-point units, so a rotation sampled
 * between two keys moves in steps of one; the same rotation reached another
 * way round (x and z either side of a half turn) is an equal matrix.
 */
#pragma once

#include "util/mat4i.h"

m4_transform_t r3d_trs_to_transform(const float translation[3], const float rotation[4], const float scale[3]);

/*
 * r3d_trs: float translation, rotation and scale as one small3dlib
 * transform, for a caller that plays an animation track (anim/) onto an
 * object drawn in fixed point.
 *
 * Translation and scale are in model units, one to S3L_F. The rotation is a
 * unit quaternion (xyzw) acting on column vectors in small3dlib's own frame,
 * and comes back as the Euler angles small3dlib composes: Z, then X, then Y,
 * in S3L turns. Angles land on whole S3L units, so a rotation sampled
 * between two keys moves in steps of one; the same rotation reached another
 * way round (x and z either side of a half turn) is an equal matrix.
 */
#pragma once

#include "render/r3d_project.h"

S3L_Transform3D r3d_transform_from_trs(const float translation[3], const float rotation[4], const float scale[3]);

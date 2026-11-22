/* hinge: a joint's local transform from a prescribed angle about its rest-frame axis, within limits. */
#pragma once

#include "math/linear/transformf.h"

typedef struct {
    transformf_t rest; /* the joint's local transform at angle 0 */
    vec3f_t axis;      /* unit, in the rest frame; positive angles turn right-handed */
    float min, max;    /* radians, min <= max */
} hinge_t;

/* Rest, then the turn about axis at angle clamped to [min, max]. Pure. */
transformf_t hinge_pose(const hinge_t* hinge, float angle);

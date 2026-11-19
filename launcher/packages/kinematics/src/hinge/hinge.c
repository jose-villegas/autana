#include "kinematics/hinge/hinge.h"

transformf_t
hinge_pose(const hinge_t* hinge, float angle) {
    transformf_t pose = hinge->rest;
    transformf_rotate(&pose, quatf_from_axis_angle(hinge->axis, mathf_clamp(angle, hinge->min, hinge->max)));
    return pose;
}

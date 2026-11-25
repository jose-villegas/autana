#include "kinematics/rig/rig.h"

/* Cinemachine treats projected directions shorter than this as zero. */
#define RIG_YAW_PROJECTION_EPSILON 1.0E-4F

vec3f_t
rig_follow_position(const transformf_t* target, vec3f_t offset, rig_bind_t bind) {
    if (bind == RIG_BIND_TARGET) {
        offset = quatf_rotate(target->rotation, offset);
    } else if (bind == RIG_BIND_TARGET_YAW) {
        const vec3f_t forward = quatf_rotate(target->rotation, vec3f_forward());
        const vec3f_t horizontal = {forward.x, 0.0F, forward.z};
        if (vec3f_dot(horizontal, horizontal) >= RIG_YAW_PROJECTION_EPSILON * RIG_YAW_PROJECTION_EPSILON) {
            offset = quatf_rotate(quatf_looking(horizontal, vec3f_up()), offset);
        }
    }
    return vec3f_add(target->position, offset);
}

quatf_t
rig_look_rotation(vec3f_t from, vec3f_t at, vec3f_t up) {
    const vec3f_t forward = vec3f_sub(at, from);
    return quatf_looking(forward, up);
}

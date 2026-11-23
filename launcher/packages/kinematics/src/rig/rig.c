#include "kinematics/rig/rig.h"

vec3f_t
rig_follow_position(const transformf_t* target, vec3f_t offset, rig_bind_t bind) {
    if (bind == RIG_BIND_TARGET) {
        offset = quatf_rotate(target->rotation, offset);
    } else if (bind == RIG_BIND_TARGET_YAW) {
        const vec3f_t up = quatf_rotate(target->rotation, vec3f_up());
        const quatf_t level = quatf_from_to(up, vec3f_up());
        offset = quatf_rotate(quatf_normalize(quatf_mul(level, target->rotation)), offset);
    }
    return vec3f_add(target->position, offset);
}

quatf_t
rig_look_rotation(vec3f_t from, vec3f_t at, vec3f_t up) {
    const vec3f_t forward = vec3f_sub(at, from);
    return quatf_looking(forward, up);
}

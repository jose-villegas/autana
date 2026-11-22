#include "kinematics/rig/rig.h"

#include "math/scalar/ease.h"

/* A projected direction this small is indistinguishable from vertical at float precision. */
#define RIG_HEADING_EPSILON 1.0E-6F

vec3f_t
rig_follow_position(transformf_t target, vec3f_t offset, rig_bind_t bind) {
    if (bind == RIG_BIND_TARGET) {
        offset = quatf_rotate(target.rotation, offset);
    } else if (bind == RIG_BIND_TARGET_YAW) {
        vec3f_t back = quatf_rotate(target.rotation, vec3f_forward());
        back.y = 0.0F;
        if (vec3f_dot(back, back) > RIG_HEADING_EPSILON * RIG_HEADING_EPSILON) {
            const quatf_t heading = transformf_looking(vec3f_zero(), back, vec3f_up()).rotation;
            offset = quatf_rotate(heading, offset);
        }
    }
    return vec3f_add(target.position, offset);
}

quatf_t
rig_look_rotation(vec3f_t from, vec3f_t at, vec3f_t up, quatf_t keep) {
    const vec3f_t back = vec3f_sub(from, at);
    if (vec3f_equal(vec3f_cross(up, back), vec3f_zero())) {
        return keep;
    }
    return transformf_looking(from, back, up).rotation;
}

float
rig_handoff(uint32_t since_ms, uint32_t blend_ms) {
    return since_ms >= blend_ms ? 1.0F : ease_smoothstep(0.0F, (float)blend_ms, (float)since_ms);
}

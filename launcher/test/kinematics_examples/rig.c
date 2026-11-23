/* Follow a sampled target pose, aim back at it, and ease in from the outgoing camera. */
#include "kinematics/rig/rig.h"
#include "math/scalar/ease.h"

transformf_t
rig_example(const transformf_t* outgoing, transformf_t target, uint32_t elapsed_ms, uint32_t over_ms) {
    const vec3f_t offset = {0.0F, 2.0F, -5.0F};
    transformf_t camera = transformf_identity();
    transformf_set_position(&camera, rig_follow_position(&target, offset, RIG_BIND_TARGET_YAW));
    transformf_set_rotation(&camera, rig_look_rotation(camera.position, target.position, vec3f_up()));
    const float weight = ease_smooth(elapsed_ms, over_ms);
    return transformf_blend(outgoing, &camera, weight);
}

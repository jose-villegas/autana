/* Pose a plate about its rest-frame z axis, within asymmetric stops. */
#include "kinematics/hinge/hinge.h"

#define PLATE_MIN_ANGLE (-0.2F)
#define PLATE_MAX_ANGLE 1.0F

transformf_t
hinge_example(float angle) {
    hinge_t plate = {
        .rest = transformf_identity(), .axis = vec3f_forward(), .min = PLATE_MIN_ANGLE, .max = PLATE_MAX_ANGLE};
    transformf_set_position(&plate.rest, vec3f_up());
    return hinge_pose(&plate, angle);
}

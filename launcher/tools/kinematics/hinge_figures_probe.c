/* hinge_figures_probe: a plate and its tip swept through hinge_pose() for the hinge reference. */
#include <stdio.h>

#include "kinematics/hinge/hinge.h"
#include "math/scalar/math_const.h"
#include "math/text/math_text.h"

#define SWEEP_STEPS 24
#define LIMIT       (MATH_PI / 3.0F)
static const vec3f_t plate[] = {{0, 0, -0.5F}, {1, 0, -0.5F}, {1, 0, 0.5F}, {0, 0, 0.5F}, {0, 0, -0.5F}};

int
main(void) {
    const hinge_t hinge = {.rest = transformf_identity(), .axis = vec3f_forward(), .min = -LIMIT, .max = LIMIT};
    printf("{\"plates\":[");
    for (int i = 0; i <= SWEEP_STEPS; i++) {
        const float angle = hinge.min + ((hinge.max - hinge.min) * (float)i / SWEEP_STEPS);
        const transformf_t pose = hinge_pose(&hinge, angle);
        printf("%s[", i ? "," : "");
        for (size_t p = 0; p < sizeof plate / sizeof plate[0]; p++) {
            printf("%s[%s]", p ? "," : "", vec3f_text(quatf_rotate(pose.rotation, plate[p])).chars);
        }
        printf("]");
    }
    puts("]}");
    return 0;
}

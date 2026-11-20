#include <math.h>
#include "suites.h"
#include "unity.h"

#include "../kinematics_examples/hinge.c"

#define TOLERANCE 1.0E-5F

static void
test_documented_hinge_stops_at_each_limit(void) {
    const float angles[] = {PLATE_MIN_ANGLE * 2.0F, PLATE_MAX_ANGLE * 2.0F};
    const float expected[] = {PLATE_MIN_ANGLE, PLATE_MAX_ANGLE};
    for (size_t i = 0; i < sizeof angles / sizeof angles[0]; i++) {
        const transformf_t pose = hinge_example(angles[i]);
        const vec3f_t tip = quatf_rotate(pose.rotation, vec3f_unit_x());
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, cosf(expected[i]), tip.x);
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, sinf(expected[i]), tip.y);
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 1.0F, pose.position.y);
    }
}

void
suite_kinematics_examples(void) {
    RUN_TEST(test_documented_hinge_stops_at_each_limit);
}

SUITE_REGISTER(suite_kinematics_examples);

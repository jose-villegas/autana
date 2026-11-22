#include <math.h>
#include "suites.h"
#include "unity.h"

#include "../kinematics_examples/hinge.c"
#include "../kinematics_examples/rig.c"

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

static void
test_documented_rig_handoff_aims_at_target(void) {
    const uint32_t duration = 1000;
    const transformf_t initial = transformf_identity();
    transformf_t target = transformf_identity();
    transformf_set_position(&target, (vec3f_t){3.0F, 4.0F, 5.0F});
    const transformf_t quarter = rig_example(&initial, target, duration / 4, duration);
    const transformf_t settled = rig_example(&initial, target, duration, duration);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.9375F, quarter.position.y);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 6.0F, settled.position.y);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, settled.position.z);
    const vec3f_t aim = quatf_rotate(settled.rotation, vec3f_forward());
    const vec3f_t expected = vec3f_normalize((vec3f_t){0.0F, -2.0F, 5.0F});
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec3f_length(vec3f_sub(aim, expected)));
}

void
suite_kinematics_examples(void) {
    RUN_TEST(test_documented_rig_handoff_aims_at_target);
    RUN_TEST(test_documented_hinge_stops_at_each_limit);
}

SUITE_REGISTER(suite_kinematics_examples);

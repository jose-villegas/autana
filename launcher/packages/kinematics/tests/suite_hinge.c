#include <math.h>

#include "kinematics/hinge/hinge.h"
#include "math/scalar/math_const.h"
#include "testkit/suite.h"
#include "unity.h"

#define TOLERANCE 1.0E-5F
#define LIMIT     (MATH_PI / 3.0F)
#define REST_TURN (MATH_PI / 4.0F)

static hinge_t
fixture(void) {
    hinge_t hinge = {.rest = transformf_identity(), .axis = vec3f_forward(), .min = -LIMIT, .max = LIMIT};
    transformf_set_position(&hinge.rest, vec3f_up());
    transformf_set_rotation(&hinge.rest, quatf_from_axis_angle(vec3f_up(), REST_TURN));
    transformf_set_scale(&hinge.rest, vec3f_scale(vec3f_one(), 2.0F));
    return hinge;
}

static void
assert_vector(vec3f_t expected, vec3f_t actual) {
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, expected.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, expected.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, expected.z, actual.z);
}

static void
test_zero_gives_rest_without_mutating_hinge(void) {
    const hinge_t hinge = fixture();
    const hinge_t before = hinge;
    const transformf_t pose = hinge_pose(&hinge, 0.0F);
    assert_vector(hinge.rest.position, pose.position);
    assert_vector(hinge.rest.scale, pose.scale);
    assert_vector(quatf_rotate(hinge.rest.rotation, vec3f_unit_x()), quatf_rotate(pose.rotation, vec3f_unit_x()));
    TEST_ASSERT_EQUAL_MEMORY(&before, &hinge, sizeof hinge);
}

static void
test_far_point_turns_in_rest_frame_and_clamps(void) {
    const hinge_t hinge = fixture();
    const float angles[] = {-2.0F * LIMIT, -LIMIT, 0.0F, LIMIT / 2.0F, LIMIT, 2.0F * LIMIT};
    for (size_t i = 0; i < sizeof angles / sizeof angles[0]; i++) {
        const float clamped = mathf_clamp(angles[i], hinge.min, hinge.max);
        const vec3f_t turned = {cosf(clamped), sinf(clamped), 0.0F};
        const vec3f_t expected =
            vec3f_add(hinge.rest.position, quatf_rotate(hinge.rest.rotation, vec3f_scale(turned, hinge.rest.scale.x)));
        const transformf_t pose = hinge_pose(&hinge, angles[i]);
        assert_vector(expected,
                      vec3f_add(pose.position, quatf_rotate(pose.rotation, vec3f_scale(vec3f_unit_x(), pose.scale.x))));
    }
}

static void
test_opposite_axes_raise_mirrored_points_together(void) {
    hinge_t left = {.rest = transformf_identity(), .axis = vec3f_forward(), .min = -LIMIT, .max = LIMIT};
    hinge_t right = left;
    transformf_set_position(&left.rest, vec3f_unit_x());
    transformf_set_position(&right.rest, vec3f_left());
    right.axis = vec3f_back();
    const transformf_t l = hinge_pose(&left, LIMIT);
    const transformf_t r = hinge_pose(&right, LIMIT);
    const vec3f_t lp = vec3f_add(l.position, quatf_rotate(l.rotation, vec3f_unit_x()));
    const vec3f_t rp = vec3f_add(r.position, quatf_rotate(r.rotation, vec3f_left()));
    assert_vector((vec3f_t){-lp.x, lp.y, lp.z}, rp);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, lp.y);
}

static void
test_asymmetric_locked_and_exact_limits(void) {
    hinge_t hinge = fixture();
    hinge.min = -0.2F;
    hinge.max = 1.0F;
    const float angles[] = {-LIMIT * 2.0F, hinge.min, hinge.max, LIMIT * 2.0F};
    const float expected[] = {hinge.min, hinge.min, hinge.max, hinge.max};
    for (size_t i = 0; i < sizeof angles / sizeof angles[0]; i++) {
        const transformf_t pose = hinge_pose(&hinge, angles[i]);
        const quatf_t rotation = quatf_mul(hinge.rest.rotation, quatf_from_axis_angle(hinge.axis, expected[i]));
        assert_vector(quatf_rotate(rotation, vec3f_unit_x()), quatf_rotate(pose.rotation, vec3f_unit_x()));
    }
    hinge.min = hinge.max;
    const transformf_t locked = hinge_pose(&hinge, hinge.min);
    for (size_t i = 0; i < sizeof angles / sizeof angles[0]; i++) {
        const transformf_t pose = hinge_pose(&hinge, angles[i]);
        assert_vector(quatf_rotate(locked.rotation, vec3f_unit_x()), quatf_rotate(pose.rotation, vec3f_unit_x()));
    }
}

void
suite_hinge(void) {
    RUN_TEST(test_asymmetric_locked_and_exact_limits);
    RUN_TEST(test_zero_gives_rest_without_mutating_hinge);
    RUN_TEST(test_far_point_turns_in_rest_frame_and_clamps);
    RUN_TEST(test_opposite_axes_raise_mirrored_points_together);
}

SUITE_REGISTER(suite_hinge);

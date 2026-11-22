#include "kinematics/rig/rig.h"
#include "math/scalar/math_const.h"
#include "testkit/suite.h"
#include "unity.h"

#define TOLERANCE    1.0E-5F
#define QUARTER_TURN (MATH_PI / 2.0F)
#define BLEND_MS     1000U

static transformf_t
fixture(void) {
    transformf_t target = transformf_identity();
    transformf_set_position(&target, (vec3f_t){2.0F, 3.0F, -4.0F});
    transformf_set_scale(&target, (vec3f_t){2.0F, 3.0F, 4.0F});
    return target;
}

static void
test_bind_frames(void) {
    transformf_t target = fixture();
    const vec3f_t offset = {1.0F, 2.0F, 3.0F};
    const float yaws[] = {-QUARTER_TURN, 0.0F, QUARTER_TURN, MATH_PI};
    const float tilts[] = {-MATH_PI / 3.0F, MATH_PI / 6.0F, MATH_PI / 4.0F};
    for (size_t i = 0; i < sizeof yaws / sizeof yaws[0]; i++) {
        const quatf_t yaw = quatf_from_axis_angle(vec3f_up(), yaws[i]);
        const vec3f_t level = vec3f_add(target.position, quatf_rotate(yaw, offset));
        const vec3f_t world = vec3f_add(target.position, offset);
        for (size_t j = 0; j < sizeof tilts / sizeof tilts[0]; j++) {
            const quatf_t pitch = quatf_from_axis_angle(vec3f_unit_x(), tilts[j]);
            const quatf_t roll = quatf_from_axis_angle(vec3f_unit_z(), -tilts[j]);
            const quatf_t rotation = quatf_mul(quatf_mul(yaw, pitch), roll);
            transformf_set_rotation(&target, rotation);
            const vec3f_t tilted = vec3f_add(target.position, quatf_rotate(rotation, offset));
            TEST_ASSERT_FLOAT_WITHIN(
                TOLERANCE, 0.0F, vec3f_length(vec3f_sub(world, rig_follow_position(target, offset, RIG_BIND_WORLD))));
            TEST_ASSERT_FLOAT_WITHIN(
                TOLERANCE, 0.0F, vec3f_length(vec3f_sub(tilted, rig_follow_position(target, offset, RIG_BIND_TARGET))));
            TEST_ASSERT_FLOAT_WITHIN(
                TOLERANCE, 0.0F,
                vec3f_length(vec3f_sub(level, rig_follow_position(target, offset, RIG_BIND_TARGET_YAW))));
        }
    }
}

static void
test_vertical_heading_uses_world_axes(void) {
    transformf_t target = fixture();
    const vec3f_t offset = {1.0F, 2.0F, 3.0F};
    const quatf_t yaw = quatf_from_axis_angle(vec3f_up(), QUARTER_TURN);
    const float pitches[] = {-QUARTER_TURN, QUARTER_TURN};
    for (size_t i = 0; i < sizeof pitches / sizeof pitches[0]; i++) {
        const quatf_t pitch = quatf_from_axis_angle(vec3f_unit_x(), pitches[i]);
        transformf_set_rotation(&target, quatf_mul(yaw, pitch));
        const vec3f_t actual = rig_follow_position(target, offset, RIG_BIND_TARGET_YAW);
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec3f_length(vec3f_sub(actual, vec3f_add(target.position, offset))));
    }
}

static void
test_look_aims_negative_z_with_level_right(void) {
    const vec3f_t from = {1.0F, -2.0F, 3.0F};
    const vec3f_t at = {-4.0F, 5.0F, 6.0F};
    const quatf_t look = rig_look_rotation(from, at, vec3f_up(), quatf_identity());
    const vec3f_t direction = vec3f_normalize(vec3f_sub(at, from));
    const vec3f_t forward = quatf_rotate(look, vec3f_back());
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec3f_length(vec3f_sub(forward, direction)));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, quatf_rotate(look, vec3f_unit_x()).y);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, quatf_rotate(look, vec3f_up()).y);
}

static void
test_look_keeps_rotation_when_points_coincide(void) {
    const vec3f_t from = vec3f_unit_x();
    const quatf_t keep = quatf_from_axis_angle(vec3f_unit_z(), QUARTER_TURN);
    const quatf_t look = rig_look_rotation(from, from, vec3f_up(), keep);
    TEST_ASSERT_EQUAL_MEMORY(&keep, &look, sizeof keep);
}

static void
test_look_keeps_rotation_at_both_poles(void) {
    const vec3f_t up = {1.0F, 2.0F, 3.0F};
    const quatf_t keep = quatf_from_axis_angle(vec3f_unit_z(), QUARTER_TURN);
    const float signs[] = {-1.0F, 1.0F};
    for (size_t i = 0; i < sizeof signs / sizeof signs[0]; i++) {
        const quatf_t look = rig_look_rotation(vec3f_zero(), vec3f_scale(up, signs[i]), up, keep);
        TEST_ASSERT_EQUAL_MEMORY(&keep, &look, sizeof keep);
    }
}

static void
test_handoff_smooths_and_clamps(void) {
    TEST_ASSERT_EQUAL_FLOAT(0.0F, rig_handoff(0, BLEND_MS));
    TEST_ASSERT_EQUAL_FLOAT(0.15625F, rig_handoff(BLEND_MS / 4, BLEND_MS));
    TEST_ASSERT_EQUAL_FLOAT(0.5F, rig_handoff(BLEND_MS / 2, BLEND_MS));
    TEST_ASSERT_EQUAL_FLOAT(1.0F, rig_handoff(BLEND_MS, BLEND_MS));
    TEST_ASSERT_EQUAL_FLOAT(1.0F, rig_handoff(UINT32_MAX, BLEND_MS));
    TEST_ASSERT_EQUAL_FLOAT(1.0F, rig_handoff(0, 0));
}

void
suite_rig(void) {
    RUN_TEST(test_bind_frames);
    RUN_TEST(test_vertical_heading_uses_world_axes);
    RUN_TEST(test_look_aims_negative_z_with_level_right);
    RUN_TEST(test_look_keeps_rotation_when_points_coincide);
    RUN_TEST(test_look_keeps_rotation_at_both_poles);
    RUN_TEST(test_handoff_smooths_and_clamps);
}

SUITE_REGISTER(suite_rig);

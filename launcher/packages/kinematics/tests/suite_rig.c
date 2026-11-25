#include <float.h>

#include "kinematics/rig/rig.h"
#include "math/scalar/math_const.h"
#include "testkit/suite.h"
#include "unity.h"

#define TOLERANCE        1.0E-5F
#define FOLLOW_TOLERANCE 5.0E-5F
#define QUARTER_TURN     (MATH_PI / 2.0F)

static const vec3f_t follow_offset = {11.0F, 23.0F, -50.0F};

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
    const quatf_t yaw = quatf_from_axis_angle(vec3f_up(), QUARTER_TURN);
    const quatf_t roll = quatf_from_axis_angle(vec3f_forward(), QUARTER_TURN);
    transformf_set_rotation(&target, quatf_mul(yaw, roll));
    const vec3f_t world = rig_follow_position(&target, follow_offset, RIG_BIND_WORLD);
    const vec3f_t full = rig_follow_position(&target, follow_offset, RIG_BIND_TARGET);
    const vec3f_t level = rig_follow_position(&target, follow_offset, RIG_BIND_TARGET_YAW);
    TEST_ASSERT_FLOAT_WITHIN(FOLLOW_TOLERANCE, 0.0F, vec3f_length(vec3f_sub(world, (vec3f_t){13, 26, -54})));
    TEST_ASSERT_FLOAT_WITHIN(FOLLOW_TOLERANCE, 0.0F, vec3f_length(vec3f_sub(full, (vec3f_t){-48, 14, 19})));
    TEST_ASSERT_FLOAT_WITHIN(FOLLOW_TOLERANCE, 0.0F, vec3f_length(vec3f_sub(level, (vec3f_t){-48, 26, -15})));
}

static void
test_follow_ignores_scale_and_zero_offset(void) {
    transformf_t scaled = fixture();
    transformf_set_position(&scaled, (vec3f_t){2.0F, -0.0F, -4.0F});
    transformf_set_rotation(&scaled, quatf_from_axis_angle(vec3f_up(), QUARTER_TURN));
    transformf_t unit = scaled;
    transformf_set_scale(&unit, vec3f_one());
    const rig_bind_t binds[] = {RIG_BIND_WORLD, RIG_BIND_TARGET, RIG_BIND_TARGET_YAW};
    for (size_t i = 0; i < sizeof binds / sizeof binds[0]; i++) {
        const vec3f_t a = rig_follow_position(&scaled, follow_offset, binds[i]);
        const vec3f_t b = rig_follow_position(&unit, follow_offset, binds[i]);
        TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof a);
        const vec3f_t origin = rig_follow_position(&scaled, vec3f_zero(), binds[i]);
        TEST_ASSERT_FLOAT_WITHIN(FOLLOW_TOLERANCE, 0.0F, vec3f_length(vec3f_sub(scaled.position, origin)));
    }
}

static void
test_yaw_bind_projects_forward_under_pitch_and_roll(void) {
    transformf_t target = fixture();
    const float tilt = QUARTER_TURN / 2.0F;
    const quatf_t pitch = quatf_from_axis_angle(vec3f_unit_x(), tilt);
    const quatf_t roll = quatf_from_axis_angle(vec3f_forward(), tilt);
    transformf_set_rotation(&target, quatf_mul(pitch, roll));
    /* Rx(tilt) Rz(tilt) maps +Z to (0, -sin(tilt), cos(tilt)). */
    const vec3f_t expected = vec3f_add(target.position, vec3f_forward());
    const vec3f_t actual = rig_follow_position(&target, vec3f_forward(), RIG_BIND_TARGET_YAW);
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec3f_length(vec3f_sub(expected, actual)));
}

static void
test_yaw_bind_follows_yaw_and_ignores_single_axis_tilt(void) {
    transformf_t target = fixture();
    const quatf_t yaw = quatf_from_axis_angle(vec3f_up(), QUARTER_TURN);
    const quatf_t tilts[] = {
        quatf_identity(),
        quatf_from_axis_angle(vec3f_unit_x(), QUARTER_TURN / 2.0F),
        quatf_from_axis_angle(vec3f_forward(), QUARTER_TURN / 2.0F),
        quatf_from_axis_angle(vec3f_forward(), MATH_PI),
    };
    const vec3f_t turned_offset = {follow_offset.z, follow_offset.y, -follow_offset.x};
    const vec3f_t expected = vec3f_add(target.position, turned_offset);
    for (size_t i = 0; i < sizeof tilts / sizeof tilts[0]; i++) {
        transformf_set_rotation(&target, quatf_mul(yaw, tilts[i]));
        const vec3f_t actual = rig_follow_position(&target, follow_offset, RIG_BIND_TARGET_YAW);
        TEST_ASSERT_FLOAT_WITHIN(FOLLOW_TOLERANCE, 0.0F, vec3f_length(vec3f_sub(expected, actual)));
    }
}

static void
test_yaw_bind_at_the_poles_uses_world_axes(void) {
    transformf_t target = fixture();
    const quatf_t yaw = quatf_from_axis_angle(vec3f_up(), QUARTER_TURN);
    const float pitches[] = {-QUARTER_TURN, QUARTER_TURN};
    const vec3f_t expected = vec3f_add(target.position, follow_offset);
    for (size_t i = 0; i < sizeof pitches / sizeof pitches[0]; i++) {
        const quatf_t pitch = quatf_from_axis_angle(vec3f_unit_x(), pitches[i]);
        transformf_set_rotation(&target, quatf_mul(yaw, pitch));
        const vec3f_t actual = rig_follow_position(&target, follow_offset, RIG_BIND_TARGET_YAW);
        TEST_ASSERT_EQUAL_MEMORY(&expected, &actual, sizeof actual);
    }
}

static void
test_look_aims_positive_z_with_level_right(void) {
    const vec3f_t from = {1.0F, -2.0F, 3.0F};
    const vec3f_t at = {-4.0F, 5.0F, 6.0F};
    const quatf_t look = rig_look_rotation(from, at, vec3f_up());
    const vec3f_t direction = vec3f_normalize(vec3f_sub(at, from));
    const vec3f_t forward = quatf_rotate(look, vec3f_forward());
    const vec3f_t right = quatf_rotate(look, vec3f_unit_x());
    const vec3f_t up = quatf_rotate(look, vec3f_up());
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec3f_length(vec3f_sub(forward, direction)));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec3f_length(vec3f_sub(vec3f_cross(right, up), forward)));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, right.y);
    TEST_ASSERT_GREATER_THAN_FLOAT(0.0F, up.y);
}

static void
test_look_respects_tilted_up_at_any_scale(void) {
    const vec3f_t up = {0.6F, 0.8F, 0.0F};
    const quatf_t near = rig_look_rotation(vec3f_zero(), vec3f_forward(), up);
    const quatf_t far =
        rig_look_rotation(vec3f_zero(), vec3f_scale(vec3f_forward(), FLT_MAX), vec3f_scale(up, 1.0E-30F));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec3f_length(vec3f_sub(quatf_rotate(near, vec3f_up()), up)));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec3f_length(vec3f_sub(quatf_rotate(far, vec3f_up()), up)));
    const vec3f_t near_right = quatf_rotate(near, vec3f_unit_x());
    const vec3f_t far_right = quatf_rotate(far, vec3f_unit_x());
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec3f_length(vec3f_sub(near_right, far_right)));
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F,
                             vec3f_length(vec3f_sub(quatf_rotate(far, vec3f_forward()), vec3f_forward())));
}

static void
test_look_coincident_points_use_identity(void) {
    const vec3f_t from = vec3f_unit_x();
    const quatf_t identity = quatf_identity();
    const quatf_t look = rig_look_rotation(from, from, vec3f_up());
    TEST_ASSERT_EQUAL_MEMORY(&identity, &look, sizeof look);
}

static void
test_look_uses_fixed_up_at_both_poles(void) {
    const vec3f_t poles[] = {{0.0F, 1.0F, 0.5F}, {0.0F, -1.0F, -0.5F}};
    const vec3f_t substitutes[] = {{0, 0, -1}, {0, 0, 1}};
    for (size_t i = 0; i < sizeof poles / sizeof poles[0]; i++) {
        const quatf_t look = rig_look_rotation(vec3f_zero(), poles[i], poles[i]);
        const vec3f_t up = quatf_rotate(look, vec3f_up());
        const vec3f_t axis = vec3f_normalize(poles[i]);
        const vec3f_t expected =
            vec3f_normalize(vec3f_sub(substitutes[i], vec3f_scale(axis, vec3f_dot(axis, substitutes[i]))));
        TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 0.0F, vec3f_length(vec3f_sub(up, expected)));
    }
}

static void
test_look_near_poles_is_finite_and_unit(void) {
    const float angles[] = {0.0F, 1.0E-30F, 1.0E-20F, 1.0E-10F, 1.0E-7F, 1.0E-6F, 1.0E-5F, 1.0E-4F, 1.0E-3F};
    const quatf_t regression = rig_look_rotation(vec3f_zero(), (vec3f_t){1.0E-30F, 5.0F, 0.0F}, vec3f_up());
    TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 1.0F,
                             regression.x * regression.x + regression.y * regression.y + regression.z * regression.z
                                 + regression.w * regression.w);
    const float poles[] = {5.0F, -5.0F};
    for (size_t i = 0; i < sizeof poles / sizeof poles[0]; i++) {
        for (size_t j = 0; j < sizeof angles / sizeof angles[0]; j++) {
            const vec3f_t at = {tanf(angles[j]) * fabsf(poles[i]), poles[i], 0};
            const quatf_t look = rig_look_rotation(vec3f_zero(), at, vec3f_up());
            TEST_ASSERT_FLOAT_WITHIN(TOLERANCE, 1.0F,
                                     sqrtf(look.x * look.x + look.y * look.y + look.z * look.z + look.w * look.w));
        }
    }
}

void
suite_rig(void) {
    RUN_TEST(test_bind_frames);
    RUN_TEST(test_follow_ignores_scale_and_zero_offset);
    RUN_TEST(test_yaw_bind_projects_forward_under_pitch_and_roll);
    RUN_TEST(test_yaw_bind_follows_yaw_and_ignores_single_axis_tilt);
    RUN_TEST(test_yaw_bind_at_the_poles_uses_world_axes);
    RUN_TEST(test_look_aims_positive_z_with_level_right);
    RUN_TEST(test_look_respects_tilted_up_at_any_scale);
    RUN_TEST(test_look_coincident_points_use_identity);
    RUN_TEST(test_look_uses_fixed_up_at_both_poles);
    RUN_TEST(test_look_near_poles_is_finite_and_unit);
}

SUITE_REGISTER(suite_rig);

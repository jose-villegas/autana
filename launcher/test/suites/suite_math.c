/*
 * Portable suite: util/math/'s float types. The quaternion's axes, slerp,
 * the matrix built from a TRS, the transform's matrix cache, the camera's
 * view matrix and look_at, each judged against a value worked out by hand.
 */

#include <math.h>

#include "suites.h"
#include "unity.h"

#include "util/math/transform.h"

#define HALF_PI 1.57079632679F
#define SLACK   1e-5F

static void
assert_vec3(vec3_t want, vec3_t got) {
    TEST_ASSERT_FLOAT_WITHIN(SLACK, want.x, got.x);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, want.y, got.y);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, want.z, got.z);
}

static void
assert_mat4(const mat4_t* want, const mat4_t* got) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            TEST_ASSERT_FLOAT_WITHIN(SLACK, want->m[r][c], got->m[r][c]);
        }
    }
}

static void
test_a_quarter_turn_about_y_takes_z_to_x(void) {
    const quat_t q = quat_from_euler((vec3_t){0.0F, HALF_PI, 0.0F});
    assert_vec3((vec3_t){1.0F, 0.0F, 0.0F}, quat_rotate(q, (vec3_t){0.0F, 0.0F, 1.0F}));
    assert_vec3((vec3_t){0.0F, 1.0F, 0.0F}, quat_rotate(q, (vec3_t){0.0F, 1.0F, 0.0F}));
}

static void
test_a_quarter_turn_about_x_takes_y_to_z_and_about_z_takes_x_to_y(void) {
    assert_vec3((vec3_t){0.0F, 0.0F, 1.0F},
                quat_rotate(quat_from_euler((vec3_t){HALF_PI, 0.0F, 0.0F}), (vec3_t){0.0F, 1.0F, 0.0F}));
    assert_vec3((vec3_t){0.0F, 1.0F, 0.0F},
                quat_rotate(quat_from_euler((vec3_t){0.0F, 0.0F, HALF_PI}), (vec3_t){1.0F, 0.0F, 0.0F}));
}

static void
test_euler_angles_apply_z_then_x_then_y(void) {
    /* +x rolls to +y about z, which a quarter turn about x then takes to +z,
     * which a quarter turn about y takes to +x. Another order ends elsewhere. */
    const quat_t q = quat_from_euler((vec3_t){HALF_PI, HALF_PI, HALF_PI});
    assert_vec3((vec3_t){1.0F, 0.0F, 0.0F}, quat_rotate(q, (vec3_t){1.0F, 0.0F, 0.0F}));
    assert_vec3((vec3_t){0.0F, 0.0F, 1.0F}, quat_rotate(q, (vec3_t){0.0F, 1.0F, 0.0F}));
}

static void
test_a_product_applies_the_right_operand_first(void) {
    const quat_t about_y = quat_from_axis_angle((vec3_t){0.0F, 1.0F, 0.0F}, HALF_PI);
    const quat_t about_x = quat_from_axis_angle((vec3_t){1.0F, 0.0F, 0.0F}, HALF_PI);
    assert_vec3((vec3_t){1.0F, 0.0F, 0.0F}, quat_rotate(quat_mul(about_y, about_x), (vec3_t){0.0F, 1.0F, 0.0F}));
}

static void
test_slerp_hits_both_ends_and_the_half_angle_between(void) {
    const quat_t a = quat_identity();
    const quat_t b = quat_from_axis_angle((vec3_t){0.0F, 1.0F, 0.0F}, HALF_PI);
    const vec3_t z = {0.0F, 0.0F, 1.0F};
    assert_vec3(quat_rotate(a, z), quat_rotate(quat_slerp(a, b, 0.0F), z));
    assert_vec3(quat_rotate(b, z), quat_rotate(quat_slerp(a, b, 1.0F), z));
    const quat_t mid = quat_slerp(a, b, 0.5F);
    assert_vec3((vec3_t){sinf(HALF_PI / 2.0F), 0.0F, cosf(HALF_PI / 2.0F)}, quat_rotate(mid, z));
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, (mid.x * mid.x) + (mid.y * mid.y) + (mid.z * mid.z) + (mid.w * mid.w));
}

static void
test_slerp_takes_the_short_way_when_the_signs_differ(void) {
    const quat_t b = quat_from_axis_angle((vec3_t){0.0F, 1.0F, 0.0F}, HALF_PI);
    const quat_t negated = {-b.x, -b.y, -b.z, -b.w};
    const quat_t mid = quat_slerp(quat_identity(), negated, 0.5F);
    assert_vec3((vec3_t){sinf(HALF_PI / 2.0F), 0.0F, cosf(HALF_PI / 2.0F)},
                quat_rotate(mid, (vec3_t){0.0F, 0.0F, 1.0F}));
}

static void
test_a_trs_matrix_is_scale_then_rotate_then_translate(void) {
    const quat_t turn = quat_from_axis_angle((vec3_t){0.0F, 1.0F, 0.0F}, HALF_PI);
    const mat4_t got = mat4_from_trs((vec3_t){1.0F, 2.0F, 3.0F}, turn, (vec3_t){2.0F, 3.0F, 4.0F});
    /* A quarter turn about y sends x to -z and z to x. */
    const mat4_t want = {{
        {0.0F, 0.0F, 4.0F, 1.0F},
        {0.0F, 3.0F, 0.0F, 2.0F},
        {-2.0F, 0.0F, 0.0F, 3.0F},
        {0.0F, 0.0F, 0.0F, 1.0F},
    }};
    assert_mat4(&want, &got);
}

static void
test_a_product_of_matrices_applies_the_right_one_first(void) {
    const mat4_t move = mat4_from_trs((vec3_t){1.0F, 0.0F, 0.0F}, quat_identity(), (vec3_t){1.0F, 1.0F, 1.0F});
    const mat4_t grow = mat4_from_trs((vec3_t){0.0F, 0.0F, 0.0F}, quat_identity(), (vec3_t){2.0F, 2.0F, 2.0F});
    const mat4_t both = mat4_mul(move, grow);
    assert_vec3((vec3_t){3.0F, 2.0F, 2.0F}, mat4_apply(&both, (vec3_t){1.0F, 1.0F, 1.0F}));
}

static void
test_the_model_matrix_is_cached_until_a_setter_runs(void) {
    transform_t t = TRANSFORM_IDENTITY;
    transform_set_position(&t, (vec3_t){1.0F, 2.0F, 3.0F});
    const mat4_t first = transform_matrix(&t);
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 1.0F, first.m[0][3]);

    t.matrix.m[0][3] = 99.0F;
    TEST_ASSERT_EQUAL_FLOAT(99.0F, transform_matrix(&t).m[0][3]);

    transform_set_position(&t, (vec3_t){4.0F, 2.0F, 3.0F});
    TEST_ASSERT_FLOAT_WITHIN(SLACK, 4.0F, transform_matrix(&t).m[0][3]);
}

static void
test_each_setter_and_translate_and_rotate_invalidate_the_cache(void) {
    transform_t t = TRANSFORM_IDENTITY;
    (void)transform_matrix(&t);
    TEST_ASSERT_FALSE(t.dirty);
    transform_set_rotation(&t, quat_identity());
    TEST_ASSERT_TRUE(t.dirty);
    (void)transform_matrix(&t);
    transform_set_scale(&t, (vec3_t){2.0F, 2.0F, 2.0F});
    TEST_ASSERT_TRUE(t.dirty);
    (void)transform_matrix(&t);
    transform_translate(&t, (vec3_t){1.0F, 0.0F, 0.0F});
    TEST_ASSERT_TRUE(t.dirty);
    (void)transform_matrix(&t);
    transform_rotate(&t, quat_from_axis_angle((vec3_t){0.0F, 1.0F, 0.0F}, HALF_PI));
    TEST_ASSERT_TRUE(t.dirty);
}

static void
test_translate_adds_and_rotate_turns_about_the_local_axes(void) {
    transform_t t = TRANSFORM_IDENTITY;
    transform_translate(&t, (vec3_t){1.0F, 0.0F, 0.0F});
    transform_translate(&t, (vec3_t){0.0F, 2.0F, 0.0F});
    assert_vec3((vec3_t){1.0F, 2.0F, 0.0F}, transform_position(&t));

    transform_rotate(&t, quat_from_axis_angle((vec3_t){0.0F, 1.0F, 0.0F}, HALF_PI));
    transform_rotate(&t, quat_from_axis_angle((vec3_t){1.0F, 0.0F, 0.0F}, HALF_PI));
    /* Local turns multiply on the right: Ry * Rx sends +z to -y. */
    assert_vec3((vec3_t){0.0F, -1.0F, 0.0F}, quat_rotate(transform_rotation(&t), (vec3_t){0.0F, 0.0F, 1.0F}));
}

static void
test_the_view_matrix_inverts_a_rigid_model_matrix(void) {
    transform_t t = TRANSFORM_IDENTITY;
    transform_set_position(&t, (vec3_t){1.0F, -2.0F, 3.0F});
    transform_set_rotation(&t, quat_from_euler((vec3_t){0.3F, -0.7F, 1.1F}));
    const mat4_t model = transform_matrix(&t);
    const mat4_t view = transform_view(&t);
    const mat4_t identity = mat4_identity();
    const mat4_t round_trip = mat4_mul(view, model);
    assert_mat4(&identity, &round_trip);
}

static void
test_the_view_matrix_puts_the_camera_at_the_origin_looking_down_z(void) {
    transform_t camera = TRANSFORM_IDENTITY;
    transform_set_position(&camera, (vec3_t){0.0F, 0.0F, -5.0F});
    const mat4_t view = transform_view(&camera);
    assert_vec3((vec3_t){0.0F, 0.0F, 5.0F}, mat4_apply(&view, (vec3_t){0.0F, 0.0F, 0.0F}));
}

static void
test_look_at_points_local_z_at_the_target_with_up_kept_up(void) {
    const vec3_t up = {0.0F, 1.0F, 0.0F};
    const vec3_t targets[] = {{0.0F, 0.0F, 5.0F}, {5.0F, 0.0F, 0.0F}, {0.0F, 3.0F, 4.0F}, {-2.0F, -1.0F, -3.0F}};
    for (unsigned i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        transform_t t = TRANSFORM_IDENTITY;
        transform_set_position(&t, (vec3_t){1.0F, 1.0F, 1.0F});
        transform_look_at(&t, targets[i], up);

        const vec3_t forward = quat_rotate(t.rotation, (vec3_t){0.0F, 0.0F, 1.0F});
        assert_vec3(vec3_normalize(vec3_sub(targets[i], t.position)), forward);

        const vec3_t right = quat_rotate(t.rotation, (vec3_t){1.0F, 0.0F, 0.0F});
        TEST_ASSERT_FLOAT_WITHIN(SLACK, 0.0F, right.y);
        TEST_ASSERT_TRUE(vec3_dot(right, vec3_cross(up, forward)) > 0.0F);
        TEST_ASSERT_TRUE(vec3_dot(quat_rotate(t.rotation, up), up) > 0.0F);
    }
}

static void
test_look_at_straight_back_is_a_half_turn(void) {
    transform_t t = TRANSFORM_IDENTITY;
    transform_look_at(&t, (vec3_t){0.0F, 0.0F, -4.0F}, (vec3_t){0.0F, 1.0F, 0.0F});
    assert_vec3((vec3_t){0.0F, 0.0F, -1.0F}, quat_rotate(t.rotation, (vec3_t){0.0F, 0.0F, 1.0F}));
    assert_vec3((vec3_t){0.0F, 1.0F, 0.0F}, quat_rotate(t.rotation, (vec3_t){0.0F, 1.0F, 0.0F}));
}

void
suite_math(void) {
    RUN_TEST(test_a_quarter_turn_about_y_takes_z_to_x);
    RUN_TEST(test_a_quarter_turn_about_x_takes_y_to_z_and_about_z_takes_x_to_y);
    RUN_TEST(test_euler_angles_apply_z_then_x_then_y);
    RUN_TEST(test_a_product_applies_the_right_operand_first);
    RUN_TEST(test_slerp_hits_both_ends_and_the_half_angle_between);
    RUN_TEST(test_slerp_takes_the_short_way_when_the_signs_differ);
    RUN_TEST(test_a_trs_matrix_is_scale_then_rotate_then_translate);
    RUN_TEST(test_a_product_of_matrices_applies_the_right_one_first);
    RUN_TEST(test_the_model_matrix_is_cached_until_a_setter_runs);
    RUN_TEST(test_each_setter_and_translate_and_rotate_invalidate_the_cache);
    RUN_TEST(test_translate_adds_and_rotate_turns_about_the_local_axes);
    RUN_TEST(test_the_view_matrix_inverts_a_rigid_model_matrix);
    RUN_TEST(test_the_view_matrix_puts_the_camera_at_the_origin_looking_down_z);
    RUN_TEST(test_look_at_points_local_z_at_the_target_with_up_kept_up);
    RUN_TEST(test_look_at_straight_back_is_a_half_turn);
}

SUITE_REGISTER(suite_math);

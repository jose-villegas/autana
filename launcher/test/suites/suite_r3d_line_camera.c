/*
 * Portable suite: the line camera, its viewport fit.
 * Each check supplies its own pose, length unit and viewport.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "render/r3d_line_camera.h"

/* r3d_line_camera_view() */

static r3d_line_camera_t
camera_fixture(void) {
    r3d_line_camera_t camera = {.pose = TRANSFORMF_IDENTITY, .focal = 1.0F, .near_z = R3D_LINE_NEAR_Z};
    return camera;
}

static void
test_an_on_axis_point_lands_on_the_viewport_centre(void) {
    transformf_t model = TRANSFORMF_IDENTITY;
    const viewport_t viewport = {.width = 368, .height = 448, .quarter = 0};
    const r3d_line_view_t view = r3d_line_camera_view(camera_fixture(), &model, viewport);

    const vec3f_t p = r3d_to_camera_space((vec3f_t){0.0F, 0.0F, 5.0F}, &view);
    int x, y;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p, &view, &x, &y));
    TEST_ASSERT_EQUAL_INT(view.center_x, x);
    TEST_ASSERT_EQUAL_INT(view.center_y, y);
}

static void
assert_near(vec3f_t want, vec3f_t got) {
    TEST_ASSERT_FLOAT_WITHIN(1e-4F, want.x, got.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4F, want.y, got.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4F, want.z, got.z);
}

/* Independent of the matrices: a model point goes to the world through the
 * model's scale, rotation and position, then into the camera by undoing the
 * camera's position and rotation, each step by quaternion and vector maths. */
static vec3f_t
camera_space_by_hand(transformf_t camera, transformf_t model, vec3f_t point) {
    const vec3f_t scaled = {point.x * model.scale.x, point.y * model.scale.y, point.z * model.scale.z};
    const vec3f_t world = vec3f_add(quatf_rotate(model.rotation, scaled), model.position);
    const quatf_t inverse = {-camera.rotation.x, -camera.rotation.y, -camera.rotation.z, camera.rotation.w};
    return quatf_rotate(inverse, vec3f_sub(world, camera.position));
}

static void
check_view_matrix_matches_hand_built(transformf_t camera_pose, transformf_t model) {
    const r3d_line_camera_t camera = {.pose = camera_pose, .focal = 1.0F, .near_z = R3D_LINE_NEAR_Z};
    const viewport_t viewport = {.width = 368, .height = 448, .quarter = 0};

    const r3d_line_view_t got = r3d_line_camera_view(camera, &model, viewport);

    const vec3f_t points[] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 2.0F, 0.0F}, {0.5F, -1.0F, 3.0F}};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        assert_near(camera_space_by_hand(camera_pose, model, points[i]), r3d_to_camera_space(points[i], &got));
    }
}

static void
test_view_matrix_matches_the_hand_built_one_for_two_unrelated_poses(void) {
    transformf_t camera_a = TRANSFORMF_IDENTITY;
    transformf_set_position(&camera_a, (vec3f_t){5.0F, 0.0F, 0.0F});
    transformf_set_rotation(&camera_a, quatf_from_euler((vec3f_t){0.0F, MATH_TAU / 8.0F, 0.0F}));
    transformf_t model_a = TRANSFORMF_IDENTITY;
    transformf_set_position(&model_a, (vec3f_t){0.0F, 0.0F, 3.0F});
    check_view_matrix_matches_hand_built(camera_a, model_a);

    transformf_t camera_b = TRANSFORMF_IDENTITY;
    transformf_set_position(&camera_b, (vec3f_t){0.0F, -2.0F, 0.0F});
    transformf_set_rotation(&camera_b, quatf_from_euler((vec3f_t){MATH_TAU / 6.0F, 0.0F, MATH_TAU / 3.0F}));
    transformf_t model_b = TRANSFORMF_IDENTITY;
    transformf_set_rotation(&model_b, quatf_from_euler((vec3f_t){0.0F, MATH_TAU / 5.0F, 0.0F}));
    transformf_set_scale(&model_b, (vec3f_t){2.0F, 2.0F, 2.0F});
    check_view_matrix_matches_hand_built(camera_b, model_b);
}

static void
check_viewport_centre_and_scale(int width, int height, int expect_center_x, int expect_center_y, float expect_scale) {
    transformf_t model = TRANSFORMF_IDENTITY;
    const viewport_t viewport = {.width = width, .height = height, .quarter = 0};

    const r3d_line_view_t view = r3d_line_camera_view(camera_fixture(), &model, viewport);

    TEST_ASSERT_EQUAL_INT(expect_center_x, view.center_x);
    TEST_ASSERT_EQUAL_INT(expect_center_y, view.center_y);
    TEST_ASSERT_EQUAL_FLOAT(expect_scale, view.scale);
}

static void
test_viewport_centre_and_scale_fit_the_shorter_axis(void) {
    check_viewport_centre_and_scale(300, 400, 150, 200, 150.0F); /* portrait: height is longer */
    check_viewport_centre_and_scale(400, 300, 200, 150, 150.0F); /* landscape: width is longer */
}

void
run_r3d_line_camera_suite(void) {
    RUN_TEST(test_an_on_axis_point_lands_on_the_viewport_centre);
    RUN_TEST(test_view_matrix_matches_the_hand_built_one_for_two_unrelated_poses);
    RUN_TEST(test_viewport_centre_and_scale_fit_the_shorter_axis);
}

SUITE_REGISTER(run_r3d_line_camera_suite);

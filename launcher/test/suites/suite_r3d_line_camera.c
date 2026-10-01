/*
 * Portable suite: render/r3d_line_camera.h (the line camera and its
 * viewport fit) and render/ray.h (the float ray camera). Both are
 * header-only and free of any particular caller's resolution, quarter or
 * unit choice, so every check here builds its own camera and viewport
 * rather than reading one a scene authored.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "render/r3d_line_camera.h"
#include "render/ray.h"

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

/* r3d_line_camera_upright() */

static void
check_up_point_moves_toward_the_quarters_own_edge(int quarter, int expect_dx_sign, int expect_dy_sign) {
    transformf_t model = TRANSFORMF_IDENTITY;
    const r3d_line_camera_t camera = r3d_line_camera_upright(camera_fixture(), quarter);
    const viewport_t viewport = {.width = 368, .height = 448, .quarter = 0};
    const r3d_line_view_t view = r3d_line_camera_view(camera, &model, viewport);

    const vec3f_t target = r3d_to_camera_space((vec3f_t){0.0F, 0.0F, 5.0F}, &view);
    const vec3f_t up = r3d_to_camera_space((vec3f_t){0.0F, 1.0F, 5.0F}, &view);
    int tx, ty, ux, uy;
    TEST_ASSERT_TRUE(r3d_project_point_cs(target, &view, &tx, &ty));
    TEST_ASSERT_TRUE(r3d_project_point_cs(up, &view, &ux, &uy));

    const int dx = ux - tx;
    const int dy = uy - ty;

    if (expect_dx_sign == 0) {
        TEST_ASSERT_EQUAL_INT(0, dx);
    } else {
        TEST_ASSERT_TRUE(expect_dx_sign > 0 ? dx > 0 : dx < 0);
    }
    if (expect_dy_sign == 0) {
        TEST_ASSERT_EQUAL_INT(0, dy);
    } else {
        TEST_ASSERT_TRUE(expect_dy_sign > 0 ? dy > 0 : dy < 0);
    }
}

/* Screen y grows downward, so "up" moving toward the panel's TOP edge is a
 * negative dy; quarter 1..3 roll that same world point toward the right,
 * bottom and left edges in turn. */
static void
test_a_point_above_the_target_projects_toward_each_quarters_own_up_edge(void) {
    check_up_point_moves_toward_the_quarters_own_edge(0, 0, -1);
    check_up_point_moves_toward_the_quarters_own_edge(1, 1, 0);
    check_up_point_moves_toward_the_quarters_own_edge(2, 0, 1);
    check_up_point_moves_toward_the_quarters_own_edge(3, -1, 0);
}

/* ray_direction() */

#define RAY_WIDTH  101 /* odd: a pixel centre lands exactly on the optical axis */
#define RAY_HEIGHT 81

static ray_camera_t
ray_camera_fixture(viewport_t viewport, float half_fov_short_tan) {
    ray_camera_t cam;
    ray_camera_init(&cam, (vec3f_t){0, 0, 0}, (vec3f_t){0, 0, 1}, (vec3f_t){1, 0, 0}, (vec3f_t){0, 1, 0},
                    half_fov_short_tan, viewport);
    return cam;
}

static void
test_the_centre_pixel_ray_is_the_forward_axis(void) {
    const viewport_t viewport = {.width = RAY_WIDTH, .height = RAY_HEIGHT, .quarter = 0};
    const ray_camera_t cam = ray_camera_fixture(viewport, 0.7f);

    const vec3f_t dir = ray_direction(&cam, RAY_WIDTH / 2, RAY_HEIGHT / 2);

    TEST_ASSERT_FLOAT_WITHIN(0.0001f, cam.forward.x, dir.x);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, cam.forward.y, dir.y);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, cam.forward.z, dir.z);
}

static void
test_the_four_corner_rays_are_symmetric_about_the_centre(void) {
    const viewport_t viewport = {.width = RAY_WIDTH, .height = RAY_HEIGHT, .quarter = 0};
    const ray_camera_t cam = ray_camera_fixture(viewport, 0.7f);

    const vec3f_t tl = ray_direction(&cam, 0, 0);
    const vec3f_t tr = ray_direction(&cam, RAY_WIDTH - 1, 0);
    const vec3f_t bl = ray_direction(&cam, 0, RAY_HEIGHT - 1);
    const vec3f_t br = ray_direction(&cam, RAY_WIDTH - 1, RAY_HEIGHT - 1);

    const float tolerance = 0.0005f;
    TEST_ASSERT_FLOAT_WITHIN(tolerance, -vec3f_dot(tl, cam.right), vec3f_dot(tr, cam.right));
    TEST_ASSERT_FLOAT_WITHIN(tolerance, vec3f_dot(tl, cam.up), vec3f_dot(tr, cam.up));
    TEST_ASSERT_FLOAT_WITHIN(tolerance, -vec3f_dot(tl, cam.up), vec3f_dot(bl, cam.up));
    TEST_ASSERT_FLOAT_WITHIN(tolerance, vec3f_dot(tl, cam.right), vec3f_dot(bl, cam.right));
    TEST_ASSERT_FLOAT_WITHIN(tolerance, -vec3f_dot(tl, cam.right), vec3f_dot(br, cam.right));
    TEST_ASSERT_FLOAT_WITHIN(tolerance, -vec3f_dot(tl, cam.up), vec3f_dot(br, cam.up));
}

/* The longer axis's own size must not change what the shorter axis sees:
 * two viewports sharing one axis's length but differing in the other's,
 * both landscape (or both portrait), read the same ray along the pixel
 * column/row exactly on the optical axis of the varying axis. */
static void
test_the_shorter_axis_alone_sets_the_lens_tangent_landscape(void) {
    const viewport_t viewport_a = {.width = 201, .height = 101, .quarter = 0}; /* aspect 1.99 */
    const viewport_t viewport_b = {.width = 351, .height = 101, .quarter = 0}; /* aspect 3.48 */
    const ray_camera_t cam_a = ray_camera_fixture(viewport_a, 0.6f);
    const ray_camera_t cam_b = ray_camera_fixture(viewport_b, 0.6f);

    const vec3f_t a = ray_direction(&cam_a, 100, 0); /* (201-1)/2 = 100: exactly on-axis */
    const vec3f_t b = ray_direction(&cam_b, 175, 0); /* (351-1)/2 = 175: exactly on-axis */

    TEST_ASSERT_EQUAL_FLOAT(a.x, b.x);
    TEST_ASSERT_EQUAL_FLOAT(a.y, b.y);
    TEST_ASSERT_EQUAL_FLOAT(a.z, b.z);
}

static void
test_the_shorter_axis_alone_sets_the_lens_tangent_portrait(void) {
    const viewport_t viewport_a = {.width = 101, .height = 201, .quarter = 0};
    const viewport_t viewport_b = {.width = 101, .height = 351, .quarter = 0};
    const ray_camera_t cam_a = ray_camera_fixture(viewport_a, 0.6f);
    const ray_camera_t cam_b = ray_camera_fixture(viewport_b, 0.6f);

    const vec3f_t a = ray_direction(&cam_a, 0, 100);
    const vec3f_t b = ray_direction(&cam_b, 0, 175);

    TEST_ASSERT_EQUAL_FLOAT(a.x, b.x);
    TEST_ASSERT_EQUAL_FLOAT(a.y, b.y);
    TEST_ASSERT_EQUAL_FLOAT(a.z, b.z);
}

/* The physical pixel that viewport_physical_to_upright() maps to the upright
 * picture's top-centre, worked out by inverting that mapping by hand,
 * an independent check, not a call into the code under test. */
static void
physical_for_upright_top_centre(int quarter, int width, int height, int* px, int* py) {
    const int eff_width = (quarter & 1) ? height : width;
    const int ux = eff_width / 2;

    switch (quarter) {
        case 1:
            *px = width - 1;
            *py = ux;
            break;
        case 2:
            *px = width - 1 - ux;
            *py = height - 1;
            break;
        case 3:
            *px = 0;
            *py = height - 1 - ux;
            break;
        default:
            *px = ux;
            *py = 0;
            break;
    }
}

static void
test_the_upright_top_centre_pixel_has_a_positive_up_component(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        const viewport_t viewport = {.width = RAY_WIDTH, .height = RAY_HEIGHT, .quarter = quarter};
        const ray_camera_t cam = ray_camera_fixture(viewport, 0.7f);

        int px, py;
        physical_for_upright_top_centre(quarter, RAY_WIDTH, RAY_HEIGHT, &px, &py);

        const vec3f_t dir = ray_direction(&cam, px, py);
        TEST_ASSERT_TRUE_MESSAGE(vec3f_dot(dir, cam.up) > 0.0f,
                                 "the upright picture's top-centre pixel must read as looking upward");
    }
}

static void
test_a_ray_direction_is_unit_length(void) {
    const viewport_t viewport = {.width = RAY_WIDTH, .height = RAY_HEIGHT, .quarter = 2};
    const ray_camera_t cam = ray_camera_fixture(viewport, 0.9f);

    const int xs[] = {0, RAY_WIDTH / 3, RAY_WIDTH - 1};
    const int ys[] = {0, RAY_HEIGHT / 2, RAY_HEIGHT - 1};

    for (size_t i = 0; i < sizeof(xs) / sizeof(xs[0]); i++) {
        const vec3f_t dir = ray_direction(&cam, xs[i], ys[i]);
        const float length = sqrtf(vec3f_dot(dir, dir));
        TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, length);
    }
}

void
run_r3d_line_camera_suite(void) {
    RUN_TEST(test_an_on_axis_point_lands_on_the_viewport_centre);
    RUN_TEST(test_view_matrix_matches_the_hand_built_one_for_two_unrelated_poses);
    RUN_TEST(test_viewport_centre_and_scale_fit_the_shorter_axis);
    RUN_TEST(test_a_point_above_the_target_projects_toward_each_quarters_own_up_edge);

    RUN_TEST(test_the_centre_pixel_ray_is_the_forward_axis);
    RUN_TEST(test_the_four_corner_rays_are_symmetric_about_the_centre);
    RUN_TEST(test_the_shorter_axis_alone_sets_the_lens_tangent_landscape);
    RUN_TEST(test_the_shorter_axis_alone_sets_the_lens_tangent_portrait);
    RUN_TEST(test_the_upright_top_centre_pixel_has_a_positive_up_component);
    RUN_TEST(test_a_ray_direction_is_unit_length);
}

SUITE_REGISTER(run_r3d_line_camera_suite);

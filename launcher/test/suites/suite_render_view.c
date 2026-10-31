/* render_view: pose basis, panel turns and perspective fit without raster scratch. */
#include "math/scalar/math_const.h"
#include "suites.h"
#include "unity.h"

#include "math/scalar/mathf.h"
#include "render/r3d_pipeline.h"
#include "render/render_view.h"

#define WIDTH          80
#define HEIGHT         60
#define HALF_FOV_TAN   0.75F
#define NEAR_Z         2.5F
#define EPSILON        0.00001F
#define AHEAD_Z        10.0F
#define POSITION_SCALE 8
#define OFFSET_X       2.0F
#define OFFSET_Y       1.0F
/* This suite's lens shift, in picture short sides, and a size to refit to. */
#define SHIFT_RIGHT    0.2F
#define SHIFT_UP       -0.15F
#define FIT_WIDTH      40
#define FIT_HEIGHT     30

static transformf_t
fixture(void) {
    transformf_t pose = TRANSFORMF_IDENTITY;
    pose.position = (vec3f_t){3.0F, 4.0F, 5.0F};
    pose.rotation = quatf_from_euler((vec3f_t){0.3F, 0.7F, 0.2F});
    pose.scale = (vec3f_t){2.0F, 3.0F, 4.0F};
    return pose;
}

static render_view_t
view_of(const transformf_t* pose, int quarter) {
    return render_view_make(pose, HALF_FOV_TAN, NEAR_Z, (viewport_t){WIDTH, HEIGHT, quarter});
}

static void
expect_vector(vec3f_t expected, vec3f_t actual) {
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, expected.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, expected.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(EPSILON, expected.z, actual.z);
}

static void
test_basis_is_orthonormal_and_picture_right_is_pose_plus_x(void) {
    const transformf_t pose = fixture();
    const render_view_t view = view_of(&pose, 0);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_SMOOTH.ceiling, view.small_fill.ceiling);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_SMOOTH.gate, view.small_fill.gate);
    TEST_ASSERT_EQUAL_INT(SMALL_FILL_SMOOTH.style, view.small_fill.style);
    expect_vector((vec3f_t){1.0F, 1.0F, 1.0F},
                  (vec3f_t){vec3f_dot(view.screen_x, view.screen_x), vec3f_dot(view.screen_y, view.screen_y),
                            vec3f_dot(view.forward, view.forward)});
    expect_vector((vec3f_t){0.0F, 0.0F, 0.0F},
                  (vec3f_t){vec3f_dot(view.screen_x, view.screen_y), vec3f_dot(view.screen_x, view.forward),
                            vec3f_dot(view.screen_y, view.forward)});
    expect_vector(view.forward, vec3f_cross(view.screen_y, view.screen_x));
    expect_vector(quatf_rotate(pose.rotation, (vec3f_t){1.0F, 0.0F, 0.0F}), view.screen_x);
    expect_vector(pose.position, view.position);
}

static void
test_quarter_turns_follow_viewport_axes(void) {
    const transformf_t pose = fixture();
    const render_view_t upright = view_of(&pose, 0);
    for (int quarter = 0; quarter < 4; quarter++) {
        const render_view_t view = view_of(&pose, quarter);
        const viewport_quarter_axes_t axes = viewport_quarter_axes(quarter);
        expect_vector(vec3f_add(vec3f_scale(upright.screen_x, (float)axes.x_right),
                                vec3f_scale(upright.screen_y, (float)axes.x_down)),
                      view.screen_x);
        expect_vector(vec3f_add(vec3f_scale(upright.screen_x, (float)axes.y_right),
                                vec3f_scale(upright.screen_y, (float)axes.y_down)),
                      view.screen_y);
        expect_vector(upright.forward, view.forward);
    }
}

static void
test_lens_fits_the_shorter_side(void) {
    const transformf_t pose = fixture();
    for (int quarter = 0; quarter < 4; quarter++) {
        for (int swapped = 0; swapped < 2; swapped++) {
            const viewport_t viewport = {swapped ? HEIGHT : WIDTH, swapped ? WIDTH : HEIGHT, quarter};
            const render_view_t view = render_view_make(&pose, HALF_FOV_TAN, NEAR_Z, viewport);
            TEST_ASSERT_EQUAL_FLOAT((float)HEIGHT / (2.0F * HALF_FOV_TAN), view.lens.fx);
            TEST_ASSERT_EQUAL_FLOAT((float)viewport.width * 0.5F, view.lens.cx);
            TEST_ASSERT_EQUAL_FLOAT((float)viewport.height * 0.5F, view.lens.cy);
            TEST_ASSERT_EQUAL_FLOAT(NEAR_Z, view.lens.near_z);
            TEST_ASSERT_EQUAL_INT(viewport.width, view.viewport.width);
            TEST_ASSERT_EQUAL_INT(viewport.height, view.viewport.height);
        }
    }
}

/* The point off the axis that a shifted lens frames: at depth AHEAD_Z, as far
 * against the shift as the shift moves the optical centre. */
static vec3f_t
framed_point(const transformf_t* pose, const render_view_t* view) {
    const vec3f_t right = quatf_rotate(pose->rotation, (vec3f_t){1.0F, 0.0F, 0.0F});
    const vec3f_t up = quatf_rotate(pose->rotation, (vec3f_t){0.0F, 1.0F, 0.0F});
    const float shorter = (float)(WIDTH < HEIGHT ? WIDTH : HEIGHT);
    const float per_short_side = shorter * AHEAD_Z / view->lens.fx;
    vec3f_t p = vec3f_add(view->position, vec3f_scale(view->forward, AHEAD_Z));
    p = vec3f_sub(p, vec3f_scale(right, SHIFT_RIGHT * per_short_side));
    return vec3f_sub(p, vec3f_scale(up, SHIFT_UP * per_short_side));
}

/* A window: a shifted lens puts a point off its axis at the picture's
 * middle, whichever way the picture is turned, and keeps it there at
 * another render size. */
static void
test_a_shifted_lens_frames_a_point_off_its_axis(void) {
    const transformf_t pose = fixture();
    for (int quarter = 0; quarter < 4; quarter++) {
        render_view_t view = view_of(&pose, quarter);
        render_view_set_shift(&view, (vec2f_t){SHIFT_RIGHT, SHIFT_UP});
        r3d_lens_t lens;
        r3d_lens_init(&lens, &view, 1);
        const vec3f_t point = framed_point(&pose, &view);
        vec2f_t screen = projectf_screen(&lens.project, mat4f_apply(&lens.m, point));
        TEST_ASSERT_FLOAT_WITHIN(1e-3F, 0.5F * (float)view.viewport.width, screen.x);
        TEST_ASSERT_FLOAT_WITHIN(1e-3F, 0.5F * (float)view.viewport.height, screen.y);
        const int fit_w = view.viewport.width == WIDTH ? FIT_WIDTH : FIT_HEIGHT;
        const int fit_h = view.viewport.width == WIDTH ? FIT_HEIGHT : FIT_WIDTH;
        r3d_lens_fit(&lens, fit_w, fit_h);
        screen = projectf_screen(&lens.project, mat4f_apply(&lens.m, point));
        TEST_ASSERT_FLOAT_WITHIN(1e-3F, 0.5F * (float)fit_w, screen.x);
        TEST_ASSERT_FLOAT_WITHIN(1e-3F, 0.5F * (float)fit_h, screen.y);
    }
}

static void
test_point_straight_ahead_projects_to_center(void) {
    const transformf_t pose = fixture();
    for (int quarter = 0; quarter < 4; quarter++) {
        const render_view_t view = view_of(&pose, quarter);
        r3d_lens_t lens;
        r3d_lens_init(&lens, &view, 1);
        const vec2f_t screen = projectf_screen(
            &lens.project, mat4f_apply(&lens.m, vec3f_add(view.position, vec3f_scale(view.forward, AHEAD_Z))));
        TEST_ASSERT_FLOAT_WITHIN(EPSILON, view.lens.cx, screen.x);
        TEST_ASSERT_FLOAT_WITHIN(EPSILON, view.lens.cy, screen.y);
    }
}

static void
test_off_axis_lens_projection_and_anisotropic_fit(void) {
    static const float offsets[4][2] = {
        {OFFSET_X, -OFFSET_Y}, {OFFSET_Y, OFFSET_X}, {-OFFSET_X, OFFSET_Y}, {-OFFSET_Y, -OFFSET_X}};
    transformf_t pose = TRANSFORMF_IDENTITY;
    pose.position = (vec3f_t){3.0F, 4.0F, 5.0F};
    const vec3f_t ticks = vec3f_scale(vec3f_add(pose.position, (vec3f_t){OFFSET_X, OFFSET_Y, AHEAD_Z}), POSITION_SCALE);
    for (int swapped = 0; swapped < 2; swapped++) {
        const int width = swapped ? HEIGHT : WIDTH;
        const int height = swapped ? WIDTH : HEIGHT;
        for (int quarter = 0; quarter < 4; quarter++) {
            const render_view_t view =
                render_view_make(&pose, HALF_FOV_TAN, NEAR_Z, (viewport_t){width, height, quarter});
            r3d_lens_t lens;
            r3d_lens_init(&lens, &view, POSITION_SCALE);
            const float focal = (float)HEIGHT / (2.0F * HALF_FOV_TAN);
            const float x = (float)width * 0.5F + focal * offsets[quarter][0] / AHEAD_Z;
            const float y = (float)height * 0.5F + focal * offsets[quarter][1] / AHEAD_Z;
            vec2f_t screen = projectf_screen(&lens.project, mat4f_apply(&lens.m, ticks));
            TEST_ASSERT_FLOAT_WITHIN(EPSILON, x, screen.x);
            TEST_ASSERT_FLOAT_WITHIN(EPSILON, y, screen.y);
            TEST_ASSERT_EQUAL_FLOAT(NEAR_Z, lens.project.near_z);
            TEST_ASSERT_EQUAL_FLOAT(NEAR_Z / (float)R3D_SUBPIXEL, lens.depth_subpixels);
            r3d_lens_fit(&lens, width / 2, height / 4);
            screen = projectf_screen(&lens.project, mat4f_apply(&lens.m, ticks));
            TEST_ASSERT_FLOAT_WITHIN(EPSILON, x * 0.5F, screen.x);
            TEST_ASSERT_FLOAT_WITHIN(EPSILON, y * 0.25F, screen.y);
        }
    }
}

static void
test_pose_roll_rotates_the_picture(void) {
    transformf_t pose = TRANSFORMF_IDENTITY;
    const render_view_t upright = view_of(&pose, 0);
    pose.rotation = quatf_from_axis_angle(upright.forward, MATH_TAU * 0.25F);
    const render_view_t rolled = view_of(&pose, 0);
    expect_vector(vec3f_scale(upright.screen_y, -1.0F), rolled.screen_x);
    expect_vector(upright.screen_x, rolled.screen_y);
    expect_vector(upright.forward, rolled.forward);
}

void
run_render_view_suite(void) {
    RUN_TEST(test_basis_is_orthonormal_and_picture_right_is_pose_plus_x);
    RUN_TEST(test_quarter_turns_follow_viewport_axes);
    RUN_TEST(test_lens_fits_the_shorter_side);
    RUN_TEST(test_point_straight_ahead_projects_to_center);
    RUN_TEST(test_a_shifted_lens_frames_a_point_off_its_axis);
    RUN_TEST(test_off_axis_lens_projection_and_anisotropic_fit);
    RUN_TEST(test_pose_roll_rotates_the_picture);
}

SUITE_REGISTER(run_render_view_suite);

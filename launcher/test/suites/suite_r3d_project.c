/* Portable line projection reference, matrix composition and near clipping. */

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "math/scalar/math_const.h"
#include "suites.h"
#include "unity.h"

#include "render/r3d_pipeline.h"
#include "render/r3d_project.h"
#include "render/r3d_project_x.h"

static render_view_t
fixture(void) {
    const transformf_t pose = TRANSFORMF_IDENTITY;
    return render_view_make(&pose, 1.0F, R3D_LINE_NEAR_Z, (viewport_t){368, 448, 0});
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
    const viewport_t viewport = {.width = 368, .height = 448, .quarter = 0};

    const render_view_t view = render_view_make(&camera_pose, 1.0F, R3D_LINE_NEAR_Z, viewport);
    const mat4f_t got = r3d_line_matrix(&view, &model);

    const vec3f_t points[] = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 2.0F, 0.0F}, {0.5F, -1.0F, 3.0F}};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        assert_near(camera_space_by_hand(camera_pose, model, points[i]), mat4f_apply(&got, points[i]));
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
    transformf_set_scale(&camera_b, (vec3f_t){3.0F, 4.0F, 5.0F});
    transformf_t model_b = TRANSFORMF_IDENTITY;
    transformf_set_rotation(&model_b, quatf_from_euler((vec3f_t){0.0F, MATH_TAU / 5.0F, 0.0F}));
    transformf_set_scale(&model_b, (vec3f_t){2.0F, 3.0F, 4.0F});
    transformf_set_position(&model_b, (vec3f_t){1.0F, -3.0F, 2.0F});
    check_view_matrix_matches_hand_built(camera_b, model_b);
}

/* The screen mapping and the point clip */

static void
test_a_point_on_the_optical_axis_lands_on_center(void) {
    const render_view_t view = fixture();
    const vec3f_t p = {0, 0, 5.0F};
    int x, y;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p, &view.lens, &x, &y));
    TEST_ASSERT_EQUAL_INT(view.lens.cx, x);
    TEST_ASSERT_EQUAL_INT(view.lens.cy, y);
}

/* Divisions are chosen to be exact, so this is a real cross-check against
 * the formula rather than a restatement of it with different variable
 * names: two unrelated centre and pixels-per-unit settings, so a hardcoded output
 * cannot pass either. */
static void
check_off_axis_point_matches_the_formula(int center_x, int center_y, float pixels_per_unit, int expected_x,
                                         int expected_y) {
    render_view_t view = fixture();
    view.lens.cx = center_x;
    view.lens.cy = center_y;
    view.lens.fx = view.lens.fy = pixels_per_unit;
    const vec3f_t p = {2.0F, -4.0F, 4.0F};
    int x, y;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p, &view.lens, &x, &y));
    TEST_ASSERT_EQUAL_INT(expected_x, x);
    TEST_ASSERT_EQUAL_INT(expected_y, y);
}

static void
test_an_off_axis_point_lands_where_the_formula_says(void) {
    check_off_axis_point_matches_the_formula(184, 224, 184.0F, 184 + 92, 224 + 184);
    check_off_axis_point_matches_the_formula(50, 80, 80.0F, 50 + 40, 80 + 80);
}

static void
test_a_point_exactly_at_near_z_counts_as_behind(void) {
    const render_view_t view = fixture();
    const vec3f_t p = {0, 0, view.lens.near_z};
    int x = -1, y = -1;

    TEST_ASSERT_FALSE(r3d_project_point_cs(p, &view.lens, &x, &y));
    TEST_ASSERT_EQUAL_INT(-1, x);
    TEST_ASSERT_EQUAL_INT(-1, y);
}

/* The segment clip */

static void
test_segment_with_both_ends_behind_returns_false(void) {
    const render_view_t view = fixture();
    const vec3f_t p0 = {0, 0, 0};
    const vec3f_t p1 = {10, 10, view.lens.near_z}; /* AT near_z, not past it */
    int ax, ay, bx, by;

    TEST_ASSERT_FALSE(r3d_project_segment_cs(p0, p1, &view.lens, &ax, &ay, &bx, &by));
}

static void
assert_screen_point(const projectf_t* lens, vec3f_t point, int x, int y) {
    int expected_x, expected_y;
    r3d_project_screen(point, lens, &expected_x, &expected_y);
    TEST_ASSERT_EQUAL_INT(expected_x, x);
    TEST_ASSERT_EQUAL_INT(expected_y, y);
}

static void
test_one_end_behind_clips_to_the_near_plane_crossing(void) {
    const float near_z = 2.0F;
    const vec3f_t behind = {7.0F, -5.0F, -2.0F};
    const vec3f_t front = {-4.0F, 13.0F, 14.0F};
    const float crossing_parts = 4.0F;
    const float remaining_parts = 3.0F;
    render_view_t view = fixture();
    view.lens.near_z = near_z;
    /* The near plane is one quarter of the way from behind to front. */
    const vec3f_t crossing = {(remaining_parts * behind.x + front.x) / crossing_parts,
                              (remaining_parts * behind.y + front.y) / crossing_parts, near_z};
    for (int reverse = 0; reverse < 2; reverse++) {
        int ax, ay, bx, by;
        TEST_ASSERT_TRUE(reverse ? r3d_project_segment_cs(front, behind, &view.lens, &ax, &ay, &bx, &by)
                                 : r3d_project_segment_cs(behind, front, &view.lens, &ax, &ay, &bx, &by));
        assert_screen_point(&view.lens, crossing, reverse ? bx : ax, reverse ? by : ay);
        assert_screen_point(&view.lens, front, reverse ? ax : bx, reverse ? ay : by);
    }
}

static void
test_both_ends_in_front_matches_projecting_each_point(void) {
    const render_view_t view = fixture();
    const vec3f_t p0 = {50.0F, -30.0F, 300.0F};
    const vec3f_t p1 = {-80.0F, 60.0F, 500.0F};
    int ax, ay, bx, by;

    TEST_ASSERT_TRUE(r3d_project_segment_cs(p0, p1, &view.lens, &ax, &ay, &bx, &by));

    assert_screen_point(&view.lens, p0, ax, ay);
    assert_screen_point(&view.lens, p1, bx, by);
}

/* A point exactly at the camera plane divides by zero; the pixel is a
 * defined, far-off one, never an undefined float-to-int conversion. */
static void
test_a_point_at_the_camera_plane_projects_to_a_far_off_but_defined_pixel(void) {
    const render_view_t view = fixture();
    const vec3f_t p = {1.0F, 1.0F, 0.0F};
    int x = 0, y = 0;

    r3d_project_screen(p, &view.lens, &x, &y);

    TEST_ASSERT_TRUE(x > view.lens.cx + 10000);
    TEST_ASSERT_TRUE(y < view.lens.cy - 10000);
}

static void
test_a_nan_offset_and_a_point_at_the_axis_on_the_camera_plane_clamp(void) {
    TEST_ASSERT_EQUAL_INT((int)R3D_PIXEL_LIMIT, r3d_pixel_offset(NAN));
    render_view_t view = fixture();
    int x = 0, y = 0;
    r3d_project_screen((vec3f_t){0.0F, 0.0F, 0.0F}, &view.lens, &x, &y); /* 0 * inf is NaN */
    TEST_ASSERT_EQUAL_INT(view.lens.cx + (int)R3D_PIXEL_LIMIT, x);
    TEST_ASSERT_EQUAL_INT(view.lens.cy - (int)R3D_PIXEL_LIMIT, y);
}

static void
test_a_pixel_offset_truncates_toward_zero_on_both_sides(void) {
    TEST_ASSERT_EQUAL_INT(2, r3d_pixel_offset(2.5F));
    TEST_ASSERT_EQUAL_INT(-2, r3d_pixel_offset(-2.5F));
    TEST_ASSERT_EQUAL_INT(0, r3d_pixel_offset(0.9F));
    TEST_ASSERT_EQUAL_INT(0, r3d_pixel_offset(-0.9F));
    TEST_ASSERT_EQUAL_INT(3, r3d_pixel_offset(2.99F)); /* the bias catches a quotient a hair short */
}

/* Meters as the fixed projection's camera-space point, 1/512 m. */
static vec3x_t
q9(vec3f_t p) {
    return (vec3x_t){mathf_round_i32(p.x * (float)R3D_X_UNIT_ONE), mathf_round_i32(p.y * (float)R3D_X_UNIT_ONE),
                     mathf_round_i32(p.z * (float)R3D_X_UNIT_ONE)};
}

static void
test_the_fixed_projection_lands_within_two_pixels_of_the_float_one(void) {
    const render_view_t view = fixture();
    const transformf_t model = TRANSFORMF_IDENTITY;
    const float meters_per_unit[] = {1.0F, 1.0F, 1.0F};
    const r3d_line_projector_t view_x = r3d_line_projector_make(&view, &model, meters_per_unit);
    for (int zi = 1; zi <= 8; zi++) {
        for (int xi = -5; xi <= 5; xi++) {
            for (int yi = -5; yi <= 5; yi += 2) {
                const vec3f_t p = {(float)xi * 0.7F, (float)yi * 0.9F, (float)zi * 4.5F};
                int fx, fy, qx, qy;
                r3d_project_screen(p, &view.lens, &fx, &fy);
                r3d_camera_to_screen_x(q9(p), &view_x, &qx, &qy);
                TEST_ASSERT_INT_WITHIN(2, fx, qx);
                TEST_ASSERT_INT_WITHIN(2, fy, qy);
            }
        }
    }
}

/* The near plane rounds to the nearest 1/512 m, as the float lens would. */
static void
test_the_line_near_plane_rounds_to_the_camera_unit(void) {
    render_view_t float_view = fixture();
    const transformf_t model = TRANSFORMF_IDENTITY;
    const float meters_per_unit[] = {1.0F, 1.0F, 1.0F};
    const int whole_units = 51;
    float_view.lens.near_z = ((float)whole_units + 0.75F) / (float)R3D_X_UNIT_ONE;
    const r3d_line_projector_t view = r3d_line_projector_make(&float_view, &model, meters_per_unit);
    TEST_ASSERT_EQUAL_INT32(whole_units + 1, r3d_line_near_z(&view));
}

static void
test_the_fixed_segment_clip_and_point_test_follow_the_near_plane(void) {
    render_view_t float_view = fixture();
    float_view.lens.near_z = 1.0F;
    float_view.lens.cx = 100;
    float_view.lens.cy = 100;
    float_view.lens.fx = float_view.lens.fy = 50.0F;
    const transformf_t model = TRANSFORMF_IDENTITY;
    const float meters_per_unit[] = {1.0F, 1.0F, 1.0F};
    const r3d_line_projector_t view = r3d_line_projector_make(&float_view, &model, meters_per_unit);
    int ax, ay, bx, by;
    TEST_ASSERT_FALSE(r3d_project_point_cs_x(q9((vec3f_t){0.0F, 0.0F, 1.0F}), &view, &ax, &ay));
    TEST_ASSERT_TRUE(r3d_project_point_cs_x(q9((vec3f_t){0.0F, 0.0F, 2.0F}), &view, &ax, &ay));
    TEST_ASSERT_EQUAL_INT(100, ax);
    TEST_ASSERT_FALSE(r3d_project_segment_cs_x(q9((vec3f_t){0.0F, 0.0F, 0.5F}), q9((vec3f_t){0.0F, 0.0F, 1.0F}), &view,
                                               &ax, &ay, &bx, &by));
    /* One end behind: it is replaced by the crossing at z = 1, where x = 4 / 3. */
    TEST_ASSERT_TRUE(r3d_project_segment_cs_x(q9((vec3f_t){4.0F, 0.0F, 3.0F}), q9((vec3f_t){0.0F, 0.0F, 0.0F}), &view,
                                              &ax, &ay, &bx, &by));
    TEST_ASSERT_INT_WITHIN(1, 100 + (int)(4.0F / 3.0F * 50.0F), ax);
    TEST_ASSERT_INT_WITHIN(2, 100 + (int)(4.0F / 3.0F * 50.0F), bx);
}

static __attribute__((noinline)) void
check_line_point(const render_view_t* view, const r3d_line_projector_t* fixed, const mat4f_t* matrix,
                 const r3d_lens_t* lens, vec3f_t point) {
    const float float_bound_px = 1.0F;
    const float fixed_bound_px = 1.5F;
    /* Dot/scale and affine paths round at most 5+1+1+1 and 6+1+1 times, fused or not.
     * Cancellation requires the pre-divide magnitude rather than the output coordinate. */
    const int projection_roundings = 16;
    const float center_magnitude = fmaxf(view->lens.cx, view->lens.cy);
    const vec3f_t delta = vec3f_sub(point, view->position);
    const float z = vec3f_dot(view->forward, delta);
    const float point_magnitude = sqrtf(vec3f_dot(point, point));
    const float projection_magnitude = center_magnitude + view->lens.fx * point_magnitude / z;
    const float lens_bound_px = FLT_EPSILON * projection_magnitude * (float)projection_roundings;
    const float expected_x = view->lens.cx + view->lens.fx * vec3f_dot(view->screen_x, delta) / z;
    const float expected_y = view->lens.cy + view->lens.fx * vec3f_dot(view->screen_y, delta) / z;
    int fx, fy, qx, qy;
    TEST_ASSERT_TRUE(r3d_project_point_cs(mat4f_apply(matrix, point), &view->lens, &fx, &fy));
    const vec3x_t camera = mat4x_apply(&fixed->matrix, vec3x_from_vec3f(point));
    TEST_ASSERT_TRUE(r3d_project_point_cs_x(camera, fixed, &qx, &qy));
    TEST_ASSERT_FLOAT_WITHIN(float_bound_px, expected_x, (float)fx);
    TEST_ASSERT_FLOAT_WITHIN(float_bound_px, expected_y, (float)fy);
    TEST_ASSERT_FLOAT_WITHIN(fixed_bound_px, expected_x, (float)qx);
    TEST_ASSERT_FLOAT_WITHIN(fixed_bound_px, expected_y, (float)qy);
    const vec2f_t screen = projectf_screen(&lens->project, mat4f_apply(&lens->m, point));
    TEST_ASSERT_FLOAT_WITHIN(lens_bound_px, expected_x, screen.x);
    TEST_ASSERT_FLOAT_WITHIN(lens_bound_px, expected_y, screen.y);
}

/* Float stays within one pixel of the continuous lens; fixed within 1.5
 * pixels, including Q9 position, quotient and rounded lens quantization. */
static void
test_line_points_follow_the_view_and_raster_lens_in_every_quarter(void) {
    const float half_fov_short_tan = 0.7F;
    const float depth = 8.0F;
    const int edge_steps = 8;
    const transformf_t model = TRANSFORMF_IDENTITY;
    const float meters_per_unit[] = {1.0F, 1.0F, 1.0F};
    for (int pose_index = 0; pose_index < 2; pose_index++) {
        for (int quarter = 0; quarter < 4; quarter++) {
            transformf_t pose = TRANSFORMF_IDENTITY;
            transformf_set_position(&pose, (vec3f_t){1.0F, -2.0F, 3.0F});
            if (pose_index != 0) {
                transformf_set_rotation(&pose, quatf_from_euler((vec3f_t){MATH_TAU / 8.0F, MATH_TAU / 8.0F, 0.0F}));
            }
            const render_view_t view =
                render_view_make(&pose, half_fov_short_tan, R3D_LINE_NEAR_Z, (viewport_t){368, 448, quarter});
            const r3d_line_projector_t fixed = r3d_line_projector_make(&view, &model, meters_per_unit);
            TEST_ASSERT_EQUAL_INT32(mathf_to_x(view.lens.fx), fixed.lens.fx);
            const mat4f_t matrix = r3d_line_matrix(&view, &model);
            r3d_lens_t lens;
            r3d_lens_init(&lens, &view, 1);
            for (int ix = -edge_steps; ix <= edge_steps; ix++) {
                for (int iy = -edge_steps; iy <= edge_steps; iy++) {
                    const float dx = view.lens.cx * (float)ix / (float)edge_steps;
                    const float dy = view.lens.cy * (float)iy / (float)edge_steps;
                    const vec3f_t relative =
                        vec3f_add(vec3f_scale(view.forward, depth),
                                  vec3f_add(vec3f_scale(view.screen_x, dx * depth / view.lens.fx),
                                            vec3f_scale(view.screen_y, dy * depth / view.lens.fx)));
                    const vec3f_t point = vec3f_add(view.position, relative);
                    check_line_point(&view, &fixed, &matrix, &lens, point);
                }
            }
        }
    }
}

static void
test_narrow_matrix_rejects_entries_at_the_limits(void) {
    const render_view_t view = fixture();
    const float narrow_scale = (float)R3D_X_UNIT_ONE * (float)(1 << R3D_X_INPUT_SHIFT);
    for (int axis = 0; axis < 3; axis++) {
        const int limit = axis == 1 ? R3D_X_ENTRY_Y_LIMIT : R3D_X_ENTRY_XZ_LIMIT;
        for (int sign = -1; sign <= 1; sign += 2) {
            for (int side = -1; side <= 1; side++) {
                float meters_per_unit[] = {0.0F, 0.0F, 0.0F};
                meters_per_unit[axis] = (float)(sign * (limit + side)) / narrow_scale;
                const transformf_t model = TRANSFORMF_IDENTITY;
                const r3d_line_projector_t fixed = r3d_line_projector_make(&view, &model, meters_per_unit);
                TEST_ASSERT_EQUAL(side < 0, fixed.units_ok);
            }
        }
    }
}

static void
test_narrow_matrix_rejects_translation_at_the_limits(void) {
    const render_view_t view = fixture();
    const float narrow_scale = (float)R3D_X_UNIT_ONE * (float)(1 << R3D_X_INPUT_SHIFT);
    const float meters_per_unit[] = {0.0F, 0.0F, 0.0F};
    for (int sign = -1; sign <= 1; sign += 2) {
        for (int side = -1; side <= 1; side++) {
            transformf_t model = TRANSFORMF_IDENTITY;
            transformf_set_position(
                &model, (vec3f_t){(float)sign
                                      * (side == 0 ? (float)R3D_X_TRANSLATION_LIMIT / narrow_scale
                                                   : nextafterf((float)R3D_X_TRANSLATION_LIMIT / narrow_scale,
                                                                side < 0 ? 0.0F : INFINITY)),
                                  0.0F, 0.0F});
            const r3d_line_projector_t fixed = r3d_line_projector_make(&view, &model, meters_per_unit);
            TEST_ASSERT_EQUAL(side < 0, fixed.units_ok);
        }
    }
}

static void
test_narrow_transform_at_the_sum_bound_matches_the_wide_one(void) {
    r3d_line_projector_t view = {0};
    for (int row = 0; row < 3; row++) {
        for (int axis = 0; axis < 3; axis++) {
            const int32_t limit = axis == 1 ? R3D_X_ENTRY_Y_LIMIT : R3D_X_ENTRY_XZ_LIMIT;
            view.units[row][axis] = limit - 1;
            view.matrix.m[row][axis] = limit - 1;
        }
        view.units[row][3] = R3D_X_TRANSLATION_LIMIT - 1;
        view.matrix.m[row][3] = view.units[row][3] >> R3D_X_INPUT_SHIFT;
    }
    const int32_t input_scale = MATHX_ONE / (1 << R3D_X_INPUT_SHIFT);
    for (int sign = -1; sign <= 1; sign += 2) {
        const vec3x_t point = {sign * R3D_X_INPUT_XZ_LIMIT * input_scale, sign * R3D_X_INPUT_Y_LIMIT * input_scale,
                               sign * R3D_X_INPUT_XZ_LIMIT * input_scale};
        const vec3x_t wide = mat4x_apply(&view.matrix, point);
        const vec3x_t narrow = r3d_to_camera_space_units(&view, sign * R3D_X_INPUT_XZ_LIMIT, sign * R3D_X_INPUT_Y_LIMIT,
                                                         sign * R3D_X_INPUT_XZ_LIMIT);
        TEST_ASSERT_EQUAL_INT32(wide.x, narrow.x);
        TEST_ASSERT_EQUAL_INT32(wide.y, narrow.y);
        TEST_ASSERT_EQUAL_INT32(wide.z, narrow.z);
    }
}

void
run_r3d_project_suite(void) {
    RUN_TEST(test_view_matrix_matches_the_hand_built_one_for_two_unrelated_poses);

    RUN_TEST(test_line_points_follow_the_view_and_raster_lens_in_every_quarter);
    RUN_TEST(test_narrow_matrix_rejects_entries_at_the_limits);
    RUN_TEST(test_narrow_matrix_rejects_translation_at_the_limits);
    RUN_TEST(test_narrow_transform_at_the_sum_bound_matches_the_wide_one);
    RUN_TEST(test_a_point_on_the_optical_axis_lands_on_center);
    RUN_TEST(test_an_off_axis_point_lands_where_the_formula_says);
    RUN_TEST(test_a_point_exactly_at_near_z_counts_as_behind);
    RUN_TEST(test_a_nan_offset_and_a_point_at_the_axis_on_the_camera_plane_clamp);
    RUN_TEST(test_a_pixel_offset_truncates_toward_zero_on_both_sides);
    RUN_TEST(test_the_fixed_projection_lands_within_two_pixels_of_the_float_one);
    RUN_TEST(test_the_line_near_plane_rounds_to_the_camera_unit);
    RUN_TEST(test_the_fixed_segment_clip_and_point_test_follow_the_near_plane);
    RUN_TEST(test_a_point_at_the_camera_plane_projects_to_a_far_off_but_defined_pixel);

    RUN_TEST(test_segment_with_both_ends_behind_returns_false);
    RUN_TEST(test_one_end_behind_clips_to_the_near_plane_crossing);
    RUN_TEST(test_both_ends_in_front_matches_projecting_each_point);
}

SUITE_REGISTER(run_r3d_project_suite);

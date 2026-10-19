/* Portable line projection reference, matrix composition and near clipping. */

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "gfx/target/gfx_segment_clip.h"
#include "math/scalar/math_const.h"
#include "suites.h"
#include "unity.h"

#include "render/r3d_grid_cull_x.h"
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
    const projectf_t lens = render_view_lens(&view);
    const vec3f_t p = {0, 0, 5.0F};
    int x, y;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p, &lens, &x, &y));
    TEST_ASSERT_EQUAL_INT(view.center_x, x);
    TEST_ASSERT_EQUAL_INT(view.center_y, y);
}

/* Divisions are chosen to be exact, so this is a real cross-check against
 * the formula rather than a restatement of it with different variable
 * names: two unrelated centre and pixels-per-unit settings, so a hardcoded output
 * cannot pass either. */
static void
check_off_axis_point_matches_the_formula(int center_x, int center_y, float pixels_per_unit, int expected_x,
                                         int expected_y) {
    render_view_t view = fixture();
    view.center_x = center_x;
    view.center_y = center_y;
    view.pixels_per_unit = pixels_per_unit;
    const projectf_t lens = render_view_lens(&view);
    const vec3f_t p = {2.0F, -4.0F, 4.0F};
    int x, y;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p, &lens, &x, &y));
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
    const projectf_t lens = render_view_lens(&view);
    const vec3f_t p = {0, 0, view.near_z};
    int x = -1, y = -1;

    TEST_ASSERT_FALSE(r3d_project_point_cs(p, &lens, &x, &y));
    TEST_ASSERT_EQUAL_INT(-1, x);
    TEST_ASSERT_EQUAL_INT(-1, y);
}

/* The segment clip */

static void
test_segment_with_both_ends_behind_returns_false(void) {
    const render_view_t view = fixture();
    const projectf_t lens = render_view_lens(&view);
    const vec3f_t p0 = {0, 0, 0};
    const vec3f_t p1 = {10, 10, view.near_z}; /* AT near_z, not past it */
    int ax, ay, bx, by;

    TEST_ASSERT_FALSE(r3d_project_segment_cs(p0, p1, &lens, &ax, &ay, &bx, &by));
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
    view.near_z = near_z;
    const projectf_t lens = render_view_lens(&view);
    /* The near plane is one quarter of the way from behind to front. */
    const vec3f_t crossing = {(remaining_parts * behind.x + front.x) / crossing_parts,
                              (remaining_parts * behind.y + front.y) / crossing_parts, near_z};
    for (int reverse = 0; reverse < 2; reverse++) {
        int ax, ay, bx, by;
        TEST_ASSERT_TRUE(reverse ? r3d_project_segment_cs(front, behind, &lens, &ax, &ay, &bx, &by)
                                 : r3d_project_segment_cs(behind, front, &lens, &ax, &ay, &bx, &by));
        assert_screen_point(&lens, crossing, reverse ? bx : ax, reverse ? by : ay);
        assert_screen_point(&lens, front, reverse ? ax : bx, reverse ? ay : by);
    }
}

static void
test_both_ends_in_front_matches_projecting_each_point(void) {
    const render_view_t view = fixture();
    const projectf_t lens = render_view_lens(&view);
    const vec3f_t p0 = {50.0F, -30.0F, 300.0F};
    const vec3f_t p1 = {-80.0F, 60.0F, 500.0F};
    int ax, ay, bx, by;

    TEST_ASSERT_TRUE(r3d_project_segment_cs(p0, p1, &lens, &ax, &ay, &bx, &by));

    assert_screen_point(&lens, p0, ax, ay);
    assert_screen_point(&lens, p1, bx, by);
}

/* A point exactly at the camera plane divides by zero; the pixel is a
 * defined, far-off one, never an undefined float-to-int conversion. */
static void
test_a_point_at_the_camera_plane_projects_to_a_far_off_but_defined_pixel(void) {
    const render_view_t view = fixture();
    const projectf_t lens = render_view_lens(&view);
    const vec3f_t p = {1.0F, 1.0F, 0.0F};
    int x = 0, y = 0;

    r3d_project_screen(p, &lens, &x, &y);

    TEST_ASSERT_TRUE(x > view.center_x + 10000);
    TEST_ASSERT_TRUE(y < view.center_y - 10000);
}

static void
test_a_nan_offset_and_a_point_at_the_axis_on_the_camera_plane_clamp(void) {
    TEST_ASSERT_EQUAL_INT((int)R3D_PIXEL_LIMIT, r3d_pixel_offset(NAN));
    render_view_t view = fixture();
    const projectf_t lens = render_view_lens(&view);
    int x = 0, y = 0;
    r3d_project_screen((vec3f_t){0.0F, 0.0F, 0.0F}, &lens, &x, &y); /* 0 * inf is NaN */
    TEST_ASSERT_EQUAL_INT(view.center_x + (int)R3D_PIXEL_LIMIT, x);
    TEST_ASSERT_EQUAL_INT(view.center_y - (int)R3D_PIXEL_LIMIT, y);
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
    const projectf_t lens = render_view_lens(&view);
    const transformf_t model = TRANSFORMF_IDENTITY;
    const float meters_per_unit[] = {1.0F, 1.0F, 1.0F};
    const r3d_line_view_x_t view_x = r3d_line_view_x_make(&view, &model, meters_per_unit);
    for (int zi = 1; zi <= 8; zi++) {
        for (int xi = -5; xi <= 5; xi++) {
            for (int yi = -5; yi <= 5; yi += 2) {
                const vec3f_t p = {(float)xi * 0.7F, (float)yi * 0.9F, (float)zi * 4.5F};
                int fx, fy, qx, qy;
                r3d_project_screen(p, &lens, &fx, &fy);
                r3d_camera_to_screen_x(q9(p), &view_x, &qx, &qy);
                TEST_ASSERT_INT_WITHIN(2, fx, qx);
                TEST_ASSERT_INT_WITHIN(2, fy, qy);
            }
        }
    }
}

#define ROUNDING_STEPS 37

static void
test_the_screen_rounding_bound_holds_for_points_on_both_sides(void) {
    const render_view_t view = fixture();
    const transformf_t model = TRANSFORMF_IDENTITY;
    const float meters_per_unit[] = {1.0F, 1.0F, 1.0F};
    const r3d_line_view_x_t view_x = r3d_line_view_x_make(&view, &model, meters_per_unit);
    const int bound = r3d_x_screen_rounding_px(&view_x);
    for (int zi = 1; zi <= ROUNDING_STEPS; zi++) {
        for (int xi = -ROUNDING_STEPS; xi <= ROUNDING_STEPS; xi++) {
            const vec3x_t p = {xi * 97, -xi * 61, view_x.near_z + (zi * 29)};
            int sx, sy;
            r3d_camera_to_screen_x(p, &view_x, &sx, &sy);
            /* |s - (center +- q * ppu / z)| < bound, scaled through by z. */
            const int64_t ex = (((int64_t)(sx - view_x.center_x)) * p.z) - ((int64_t)p.x * view_x.pixels_per_unit);
            const int64_t ey = (((int64_t)(view_x.center_y - sy)) * p.z) - ((int64_t)p.y * view_x.pixels_per_unit);
            TEST_ASSERT_TRUE(ex < (int64_t)bound * p.z && -ex < (int64_t)bound * p.z);
            TEST_ASSERT_TRUE(ey < (int64_t)bound * p.z && -ey < (int64_t)bound * p.z);
        }
    }
}

static void
test_the_fixed_segment_clip_and_point_test_follow_the_near_plane(void) {
    render_view_t float_view = fixture();
    float_view.near_z = 1.0F;
    float_view.center_x = 100;
    float_view.center_y = 100;
    float_view.pixels_per_unit = 50.0F;
    const transformf_t model = TRANSFORMF_IDENTITY;
    const float meters_per_unit[] = {1.0F, 1.0F, 1.0F};
    const r3d_line_view_x_t view = r3d_line_view_x_make(&float_view, &model, meters_per_unit);
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

/* Float stays within one pixel of the continuous lens; fixed within 1.5
 * pixels, including Q9 position, quotient and rounded lens quantization. */
static void
test_line_points_follow_the_view_and_raster_lens_in_every_quarter(void) {
    const float half_fov_short_tan = 0.7F;
    const float depth = 8.0F;
    const float float_bound_px = 1.0F;
    const float fixed_bound_px = 1.5F;
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
            const r3d_line_view_x_t fixed = r3d_line_view_x_make(&view, &model, meters_per_unit);
            TEST_ASSERT_EQUAL_INT(mathf_round_i32(view.pixels_per_unit), fixed.pixels_per_unit);
            const mat4f_t matrix = r3d_line_matrix(&view, &model);
            const projectf_t projection = render_view_lens(&view);
            r3d_lens_t lens;
            r3d_lens_init(&lens, &view, 1);
            /* Dot/scale and affine paths round at most 5+1+1+1 and 6+1+1 times, fused or not.
             * Cancellation requires the pre-divide magnitude rather than the output coordinate. */
            const int projection_roundings = 16;
            const float center_magnitude = fmaxf(view.center_x, view.center_y);
            for (int ix = -edge_steps; ix <= edge_steps; ix++) {
                for (int iy = -edge_steps; iy <= edge_steps; iy++) {
                    const float dx = view.center_x * (float)ix / (float)edge_steps;
                    const float dy = view.center_y * (float)iy / (float)edge_steps;
                    const vec3f_t relative =
                        vec3f_add(vec3f_scale(view.forward, depth),
                                  vec3f_add(vec3f_scale(view.screen_x, dx * depth / view.pixels_per_unit),
                                            vec3f_scale(view.screen_y, dy * depth / view.pixels_per_unit)));
                    const vec3f_t point = vec3f_add(view.position, relative);
                    const vec3f_t delta = vec3f_sub(point, view.position);
                    const float z = vec3f_dot(view.forward, delta);
                    const float point_magnitude = sqrtf(vec3f_dot(point, point));
                    const float projection_magnitude = center_magnitude + view.pixels_per_unit * point_magnitude / z;
                    const float lens_bound_px = FLT_EPSILON * projection_magnitude * (float)projection_roundings;
                    const float expected_x = view.center_x + view.pixels_per_unit * vec3f_dot(view.screen_x, delta) / z;
                    const float expected_y = view.center_y + view.pixels_per_unit * vec3f_dot(view.screen_y, delta) / z;
                    int fx, fy, qx, qy;
                    TEST_ASSERT_TRUE(r3d_project_point_cs(mat4f_apply(&matrix, point), &projection, &fx, &fy));
                    const vec3x_t camera = mat4x_apply(&fixed.matrix, vec3x_from_vec3f(point));
                    TEST_ASSERT_TRUE(r3d_project_point_cs_x(camera, &fixed, &qx, &qy));
                    TEST_ASSERT_FLOAT_WITHIN(float_bound_px, expected_x, (float)fx);
                    TEST_ASSERT_FLOAT_WITHIN(float_bound_px, expected_y, (float)fy);
                    TEST_ASSERT_FLOAT_WITHIN(fixed_bound_px, expected_x, (float)qx);
                    TEST_ASSERT_FLOAT_WITHIN(fixed_bound_px, expected_y, (float)qy);
                    const vec3f_t lp = mat4f_apply(&lens.m, point);
                    TEST_ASSERT_FLOAT_WITHIN(lens_bound_px, expected_x, lens.center_x + lp.x / lp.z);
                    TEST_ASSERT_FLOAT_WITHIN(lens_bound_px, expected_y, lens.center_y + lp.y / lp.z);
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
                const r3d_line_view_x_t fixed = r3d_line_view_x_make(&view, &model, meters_per_unit);
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
            const r3d_line_view_x_t fixed = r3d_line_view_x_make(&view, &model, meters_per_unit);
            TEST_ASSERT_EQUAL(side < 0, fixed.units_ok);
        }
    }
}

static void
test_narrow_transform_at_the_sum_bound_matches_the_wide_one(void) {
    r3d_line_view_x_t view = {0};
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

/* The grid cull */

#define CULL_ROW_HALF 24
#define CULL_K_MAX    (2 * CULL_ROW_HALF)
#define CULL_ROWS     (CULL_K_MAX + 1)
#define CULL_INPUT_Q  4096 /* raw inputs per meter on the row's floor axes */
#define CULL_STEP     (CULL_INPUT_Q / 2)
#define CULL_LIFT_MAX 600
#define CULL_LIFTS    9 /* lifts tried per point, spread over +-CULL_LIFT_MAX */
#define CULL_YAWS     8
#define CULL_PITCHES  3

static int32_t
cull_lift(int n) {
    return -CULL_LIFT_MAX + ((2 * CULL_LIFT_MAX * n) / (CULL_LIFTS - 1));
}

/* The narrow transform's sum before its shift, which must fit an int32. */
static int32_t
cull_sum(const r3d_line_view_x_t* view, int row, int32_t a, int32_t b, int32_t c) {
    const int64_t sum = ((int64_t)view->units[row][0] * a) + ((int64_t)view->units[row][1] * b)
                        + ((int64_t)view->units[row][2] * c) + view->units[row][3];
    TEST_ASSERT_TRUE(sum >= INT32_MIN && sum <= INT32_MAX);
    return (int32_t)sum;
}

static vec3x_t
cull_point(const r3d_line_view_x_t* view, int32_t a, int32_t b, int32_t c) {
    return (vec3x_t){cull_sum(view, 0, a, b, c), cull_sum(view, 1, a, b, c), cull_sum(view, 2, a, b, c)};
}

/* Grid point (k, row) at `lift`: its outcode, or -1 at or behind the near plane. */
static int
cull_outcode(const r3d_line_view_x_t* view, const recti_t* clip, int k, int row, int32_t lift) {
    const vec3x_t p =
        r3d_to_camera_space_units(view, (k - CULL_ROW_HALF) * CULL_STEP, lift, (row - CULL_ROW_HALF) * CULL_STEP);
    int sx = 0;
    int sy = 0;
    return r3d_project_point_cs_x(p, view, &sx, &sy) ? gfx_clip_outcode(clip, sx, sy) : -1;
}

/* Every row's keep span, the grid's first point at -CULL_ROW_HALF steps on both axes. */
static __attribute__((noinline)) void
cull_spans(const r3d_line_view_x_t* view, const recti_t* clip, int8_t keep[][2]) {
    const vec3x_t base = cull_point(view, -CULL_ROW_HALF * CULL_STEP, 0, -CULL_ROW_HALF * CULL_STEP);
    const vec3x_t ahead = cull_point(view, (1 - CULL_ROW_HALF) * CULL_STEP, 0, -CULL_ROW_HALF * CULL_STEP);
    const vec3x_t below = cull_point(view, -CULL_ROW_HALF * CULL_STEP, 0, (1 - CULL_ROW_HALF) * CULL_STEP);
    const r3d_x_grid_t grid = {
        .base = base,
        .step = {ahead.x - base.x, ahead.y - base.y, ahead.z - base.z},
        .row = {below.x - base.x, below.y - base.y, below.z - base.z},
        .rise = {view->units[0][1], view->units[1][1], view->units[2][1]},
        .h_max = CULL_LIFT_MAX,
        .k_max = CULL_K_MAX,
        .r_max = CULL_K_MAX,
    };
    r3d_x_grid_cull_spans(view, clip, &grid, keep);
}

/* The edge between two points stays hidden at every pair of end lifts. */
static void
check_edge_hidden(const r3d_line_view_x_t* view, const recti_t* clip, int ka, int ra, int kb, int rb) {
    for (int na = 0; na < CULL_LIFTS; na += CULL_LIFTS - 1) {
        for (int nb = 0; nb < CULL_LIFTS; nb += CULL_LIFTS - 1) {
            const int a = cull_outcode(view, clip, ka, ra, cull_lift(na));
            const int b = cull_outcode(view, clip, kb, rb, cull_lift(nb));
            TEST_ASSERT_TRUE((a < 0 && b < 0) || (a > 0 && b > 0 && (a & b) != 0));
        }
    }
}

/* Whether a point misses the clip at every lift tried. */
static bool
cull_misses(const r3d_line_view_x_t* view, const recti_t* clip, int k, int r) {
    for (int n = 0; n < CULL_LIFTS; n++) {
        if (cull_outcode(view, clip, k, r, cull_lift(n)) == 0) {
            return false;
        }
    }
    return true;
}

/* Every point outside its row's keep span has only hidden edges; returns
 * how many points the spans drop, and adds how many miss the clip at every
 * lift tried to *missed. */
static __attribute__((noinline)) unsigned
check_cull_spans(const r3d_line_view_x_t* view, const recti_t* clip, int8_t keep[][2], unsigned* missed) {
    static const int steps[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    cull_spans(view, clip, keep);
    unsigned dropped = 0;
    for (int r = 0; r < CULL_ROWS; r++) {
        for (int k = 0; k <= CULL_K_MAX; k++) {
            *missed += cull_misses(view, clip, k, r);
            if (k >= keep[r][0] && k <= keep[r][1]) {
                continue;
            }
            dropped++;
            for (int s = 0; s < 4; s++) {
                const int kn = k + steps[s][0];
                const int rn = r + steps[s][1];
                if (kn >= 0 && kn <= CULL_K_MAX && rn >= 0 && rn < CULL_ROWS) {
                    check_edge_hidden(view, clip, k, r, kn, rn);
                }
            }
        }
    }
    return dropped;
}

/* A camera over a floor, turned by `yaw` eighths and looking down by
 * `pitch` 24ths, so rows leave the screen through each edge and pass behind
 * the camera. This and the checks are outlined: inline, together they are
 * too large for the device's test stack. */
static __attribute__((noinline)) r3d_line_view_x_t
cull_view(int yaw, int pitch) {
    const float meters_per_unit[] = {1.0F / CULL_INPUT_Q, 1.0F / (float)(1 << R3D_X_INPUT_SHIFT), 1.0F / CULL_INPUT_Q};
    const transformf_t model = TRANSFORMF_IDENTITY;
    transformf_t pose = TRANSFORMF_IDENTITY;
    const float turn = MATH_TAU * (float)yaw / CULL_YAWS;
    transformf_set_position(&pose, (vec3f_t){3.0F * sinf(turn), 1.5F * (float)pitch, -3.0F * cosf(turn)});
    transformf_set_rotation(&pose, quatf_from_euler((vec3f_t){MATH_TAU * (float)pitch / 24.0F, -turn, 0.0F}));
    const render_view_t view = render_view_make(&pose, 0.7F, R3D_LINE_NEAR_Z, (viewport_t){368, 448, 0});
    return r3d_line_view_x_make(&view, &model, meters_per_unit);
}

/* Every pose against the whole screen and an inset clip: adds the points
 * missing the clip and those the spans drop. */
static __attribute__((noinline)) void
check_cull_poses(int8_t keep[][2], unsigned* missed, unsigned* dropped) {
    const recti_t clips[] = {{0, 0, 368, 448}, {40, 60, 200, 250}};
    for (int yaw = 0; yaw < CULL_YAWS; yaw++) {
        for (int pitch = 1; pitch <= CULL_PITCHES; pitch++) {
            const r3d_line_view_x_t view = cull_view(yaw, pitch);
            TEST_ASSERT_TRUE(view.units_ok);
            for (unsigned c = 0; c < sizeof clips / sizeof clips[0]; c++) {
                *dropped += check_cull_spans(&view, &clips[c], keep, missed);
            }
        }
    }
}

static void
test_the_grid_cull_drops_only_points_whose_edges_all_project_hidden(void) {
    int8_t (*keep)[2] = malloc(sizeof(*keep) * CULL_ROWS);
    TEST_ASSERT_NOT_NULL(keep);
    unsigned missed = 0;
    unsigned dropped = 0;
    check_cull_poses(keep, &missed, &dropped);
    free(keep);
    /* The lifts, the rounding and the edges along the screen keep a margin,
     * but three in four points that miss are dropped. */
    TEST_ASSERT_GREATER_OR_EQUAL((missed * 3) / 4, dropped);
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
    RUN_TEST(test_the_fixed_segment_clip_and_point_test_follow_the_near_plane);
    RUN_TEST(test_the_screen_rounding_bound_holds_for_points_on_both_sides);
    RUN_TEST(test_a_point_at_the_camera_plane_projects_to_a_far_off_but_defined_pixel);

    RUN_TEST(test_segment_with_both_ends_behind_returns_false);
    RUN_TEST(test_one_end_behind_clips_to_the_near_plane_crossing);
    RUN_TEST(test_both_ends_in_front_matches_projecting_each_point);

    RUN_TEST(test_the_grid_cull_drops_only_points_whose_edges_all_project_hidden);
}

SUITE_REGISTER(run_r3d_project_suite);

/*
 * Portable suite: render/r3d_project.h - the camera-space near-plane clip
 * and perspective projection a caller reaches once it has already composed
 * its own model*view matrix. Header-only and free of any particular
 * caller's resolution or unit choice, so every check here builds its own
 * `r3d_view_t` rather than reading one a timeline authored.
 */

#include <stdbool.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "render/r3d_project.h"

static r3d_view_t
fixture(void) {
    r3d_view_t v;
    S3L_mat4Init(v.matrix);
    v.focal = S3L_F;
    v.near_z = R3D_NEAR_Z;
    v.center_x = 184;
    v.center_y = 224;
    v.scale = 184;
    return v;
}

/* The matrix multiply */

static void
test_to_camera_space_leaves_a_point_unchanged_under_identity(void) {
    const r3d_view_t view = fixture();
    const S3L_Vec4 p = {17, -42, 900, S3L_F};

    const S3L_Vec4 got = r3d_to_camera_space(p, &view);

    TEST_ASSERT_EQUAL_INT32(p.x, got.x);
    TEST_ASSERT_EQUAL_INT32(p.y, got.y);
    TEST_ASSERT_EQUAL_INT32(p.z, got.z);
}

/* Translation set directly on the matrix (small3dlib's own m[col][3]
 * convention), not through S3L_makeWorldMatrix's scale/rotate/translate
 * chain: composing three matrices for one axis-aligned move would fold in
 * that chain's own fixed-point rounding, which is not what this checks. */
static void
test_to_camera_space_applies_the_composed_matrix(void) {
    r3d_view_t view = fixture();
    S3L_mat4Init(view.matrix);
    view.matrix[0][3] = 10 * S3L_F;
    view.matrix[1][3] = 20 * S3L_F;
    view.matrix[2][3] = 30 * S3L_F;
    const S3L_Vec4 p = {1 * S3L_F, 2 * S3L_F, 3 * S3L_F, S3L_F};

    const S3L_Vec4 got = r3d_to_camera_space(p, &view);

    TEST_ASSERT_EQUAL_INT32(11 * S3L_F, got.x);
    TEST_ASSERT_EQUAL_INT32(22 * S3L_F, got.y);
    TEST_ASSERT_EQUAL_INT32(33 * S3L_F, got.z);
}

/* The screen mapping and the point clip */

static void
test_a_point_on_the_optical_axis_lands_on_center(void) {
    const r3d_view_t view = fixture();
    const S3L_Vec4 p = {0, 0, 5 * S3L_F, S3L_F};
    int x, y;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p, &view, &x, &y));
    TEST_ASSERT_EQUAL_INT(view.center_x, x);
    TEST_ASSERT_EQUAL_INT(view.center_y, y);
}

/* Divisions are chosen to be exact, so this is a real cross-check against
 * the formula rather than a restatement of it with different variable
 * names - two unrelated center/scale/focal settings, so a hardcoded output
 * cannot pass either. */
static void
check_off_axis_point_matches_the_formula(int center_x, int center_y, int scale, S3L_Unit focal, int expected_x,
                                         int expected_y) {
    r3d_view_t view = fixture();
    view.center_x = center_x;
    view.center_y = center_y;
    view.scale = scale;
    view.focal = focal;
    const S3L_Vec4 p = {2 * S3L_F, -4 * S3L_F, 4 * S3L_F, S3L_F};
    int x, y;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p, &view, &x, &y));
    TEST_ASSERT_EQUAL_INT(expected_x, x);
    TEST_ASSERT_EQUAL_INT(expected_y, y);
}

static void
test_an_off_axis_point_lands_where_the_formula_says(void) {
    /* focal == S3L_F, z == 4*S3L_F halves x/y; scale == center then applies
     * that half again. */
    check_off_axis_point_matches_the_formula(184, 224, 184, S3L_F, 184 + 92, 224 + 184);
    /* focal == 2*S3L_F cancels the same halving; a different scale/center
     * pair, so this is not the same arithmetic wearing a disguise. */
    check_off_axis_point_matches_the_formula(50, 80, 40, 2 * S3L_F, 50 + 40, 80 + 80);
}

static void
test_a_point_exactly_at_near_z_counts_as_behind(void) {
    const r3d_view_t view = fixture();
    const S3L_Vec4 p = {0, 0, view.near_z, S3L_F};
    int x = -1, y = -1;

    TEST_ASSERT_FALSE(r3d_project_point_cs(p, &view, &x, &y));
    TEST_ASSERT_EQUAL_INT(-1, x);
    TEST_ASSERT_EQUAL_INT(-1, y);
}

static void
test_orthographic_projection_ignores_depth(void) {
    r3d_view_t view = fixture();
    view.focal = 0;
    const S3L_Vec4 p_near = {100, -50, 2 * S3L_F, S3L_F};
    const S3L_Vec4 p_far = {100, -50, 500 * S3L_F, S3L_F};
    int x_near, y_near, x_far, y_far;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p_near, &view, &x_near, &y_near));
    TEST_ASSERT_TRUE(r3d_project_point_cs(p_far, &view, &x_far, &y_far));

    TEST_ASSERT_EQUAL_INT(x_near, x_far);
    TEST_ASSERT_EQUAL_INT(y_near, y_far);
}

/* The segment clip */

static void
test_segment_with_both_ends_behind_returns_false(void) {
    const r3d_view_t view = fixture();
    const S3L_Vec4 p0 = {0, 0, 0, S3L_F};
    const S3L_Vec4 p1 = {10, 10, view.near_z, S3L_F}; /* AT near_z, not past it */
    int ax, ay, bx, by;

    TEST_ASSERT_FALSE(r3d_project_segment_cs(p0, p1, &view, &ax, &ay, &bx, &by));
}

/* near_z sits exactly halfway between the two z's, so the crossing point is
 * plain averaging - independent of the Q16 interpolation the function under
 * test actually does - rather than a hand copy of its formula. */
static void
test_one_end_behind_clips_to_the_near_plane_crossing(void) {
    r3d_view_t view = fixture();
    view.near_z = 100;
    const S3L_Vec4 p0 = {0, 0, 0, S3L_F};
    const S3L_Vec4 p1 = {200, 100, 200, S3L_F};
    int ax, ay, bx, by;

    TEST_ASSERT_TRUE(r3d_project_segment_cs(p0, p1, &view, &ax, &ay, &bx, &by));

    const S3L_Vec4 at_near_z = {(p0.x + p1.x) / 2, (p0.y + p1.y) / 2, view.near_z, S3L_F};
    int ex, ey, fx, fy;
    r3d_camera_to_screen(at_near_z, &view, &ex, &ey);
    r3d_camera_to_screen(p1, &view, &fx, &fy);

    TEST_ASSERT_EQUAL_INT(ex, ax);
    TEST_ASSERT_EQUAL_INT(ey, ay);
    TEST_ASSERT_EQUAL_INT(fx, bx);
    TEST_ASSERT_EQUAL_INT(fy, by);
}

static void
test_both_ends_in_front_matches_projecting_each_point(void) {
    const r3d_view_t view = fixture();
    const S3L_Vec4 p0 = {50, -30, 300, S3L_F};
    const S3L_Vec4 p1 = {-80, 60, 500, S3L_F};
    int ax, ay, bx, by;

    TEST_ASSERT_TRUE(r3d_project_segment_cs(p0, p1, &view, &ax, &ay, &bx, &by));

    int ex0, ey0, ex1, ey1;
    r3d_camera_to_screen(p0, &view, &ex0, &ey0);
    r3d_camera_to_screen(p1, &view, &ex1, &ey1);

    TEST_ASSERT_EQUAL_INT(ex0, ax);
    TEST_ASSERT_EQUAL_INT(ey0, ay);
    TEST_ASSERT_EQUAL_INT(ex1, bx);
    TEST_ASSERT_EQUAL_INT(ey1, by);
}

void
run_r3d_project_suite(void) {
    RUN_TEST(test_to_camera_space_leaves_a_point_unchanged_under_identity);
    RUN_TEST(test_to_camera_space_applies_the_composed_matrix);

    RUN_TEST(test_a_point_on_the_optical_axis_lands_on_center);
    RUN_TEST(test_an_off_axis_point_lands_where_the_formula_says);
    RUN_TEST(test_a_point_exactly_at_near_z_counts_as_behind);
    RUN_TEST(test_orthographic_projection_ignores_depth);

    RUN_TEST(test_segment_with_both_ends_behind_returns_false);
    RUN_TEST(test_one_end_behind_clips_to_the_near_plane_crossing);
    RUN_TEST(test_both_ends_in_front_matches_projecting_each_point);
}

SUITE_REGISTER(run_r3d_project_suite);

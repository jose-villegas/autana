/*
 * Portable suite: render/r3d_project.h, the camera-space near-plane clip
 * and perspective projection a caller reaches once it has already composed
 * its own model*view matrix. Header-only and free of any particular
 * caller's resolution or unit choice, so every check here builds its own
 * `r3d_line_view_t` rather than reading one a timeline authored.
 */

#include <stdbool.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "render/r3d_project.h"

static r3d_line_view_t
fixture(void) {
    r3d_line_view_t v;
    v.matrix = mat4_identity();
    v.focal = 1.0F;
    v.near_z = R3D_LINE_NEAR_Z;
    v.center_x = 184;
    v.center_y = 224;
    v.scale = 184.0F;
    return v;
}

/* The matrix multiply */

static void
test_to_camera_space_leaves_a_point_unchanged_under_identity(void) {
    const r3d_line_view_t view = fixture();
    const vec3_t p = {17.0F, -42.0F, 900.0F};

    const vec3_t got = r3d_to_camera_space(p, &view);

    TEST_ASSERT_EQUAL_FLOAT(p.x, got.x);
    TEST_ASSERT_EQUAL_FLOAT(p.y, got.y);
    TEST_ASSERT_EQUAL_FLOAT(p.z, got.z);
}

/* Translation set directly on the matrix's last column, not through a
 * transform's scale/rotate/translate chain: this checks the multiply, not
 * the composition. */
static void
test_to_camera_space_applies_the_composed_matrix(void) {
    r3d_line_view_t view = fixture();
    view.matrix = mat4_identity();
    view.matrix.m[0][3] = 10.0F;
    view.matrix.m[1][3] = 20.0F;
    view.matrix.m[2][3] = 30.0F;
    const vec3_t p = {1.0F, 2.0F, 3.0F};

    const vec3_t got = r3d_to_camera_space(p, &view);

    TEST_ASSERT_EQUAL_FLOAT(11.0F, got.x);
    TEST_ASSERT_EQUAL_FLOAT(22.0F, got.y);
    TEST_ASSERT_EQUAL_FLOAT(33.0F, got.z);
}

/* The screen mapping and the point clip */

static void
test_a_point_on_the_optical_axis_lands_on_center(void) {
    const r3d_line_view_t view = fixture();
    const vec3_t p = {0, 0, 5.0F};
    int x, y;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p, &view, &x, &y));
    TEST_ASSERT_EQUAL_INT(view.center_x, x);
    TEST_ASSERT_EQUAL_INT(view.center_y, y);
}

/* Divisions are chosen to be exact, so this is a real cross-check against
 * the formula rather than a restatement of it with different variable
 * names: two unrelated center/scale/focal settings, so a hardcoded output
 * cannot pass either. */
static void
check_off_axis_point_matches_the_formula(int center_x, int center_y, float scale, float focal, int expected_x,
                                         int expected_y) {
    r3d_line_view_t view = fixture();
    view.center_x = center_x;
    view.center_y = center_y;
    view.scale = scale;
    view.focal = focal;
    const vec3_t p = {2.0F, -4.0F, 4.0F};
    int x, y;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p, &view, &x, &y));
    TEST_ASSERT_EQUAL_INT(expected_x, x);
    TEST_ASSERT_EQUAL_INT(expected_y, y);
}

static void
test_an_off_axis_point_lands_where_the_formula_says(void) {
    /* focal 1 and z 4 quarter x/y; scale == center then applies that again. */
    check_off_axis_point_matches_the_formula(184, 224, 184.0F, 1.0F, 184 + 92, 224 + 184);
    /* focal 2 halves the same ratio; a different scale/center pair, so this
     * is not the same arithmetic wearing a disguise. */
    check_off_axis_point_matches_the_formula(50, 80, 40.0F, 2.0F, 50 + 40, 80 + 80);
}

static void
test_a_point_exactly_at_near_z_counts_as_behind(void) {
    const r3d_line_view_t view = fixture();
    const vec3_t p = {0, 0, view.near_z};
    int x = -1, y = -1;

    TEST_ASSERT_FALSE(r3d_project_point_cs(p, &view, &x, &y));
    TEST_ASSERT_EQUAL_INT(-1, x);
    TEST_ASSERT_EQUAL_INT(-1, y);
}

static void
test_orthographic_projection_ignores_depth(void) {
    r3d_line_view_t view = fixture();
    view.focal = 0.0F;
    const vec3_t p_near = {100.0F, -50.0F, 2.0F};
    const vec3_t p_far = {100.0F, -50.0F, 500.0F};
    int x_near, y_near, x_far, y_far;

    TEST_ASSERT_TRUE(r3d_project_point_cs(p_near, &view, &x_near, &y_near));
    TEST_ASSERT_TRUE(r3d_project_point_cs(p_far, &view, &x_far, &y_far));

    TEST_ASSERT_EQUAL_INT(x_near, x_far);
    TEST_ASSERT_EQUAL_INT(y_near, y_far);
}

/* The segment clip */

static void
test_segment_with_both_ends_behind_returns_false(void) {
    const r3d_line_view_t view = fixture();
    const vec3_t p0 = {0, 0, 0};
    const vec3_t p1 = {10, 10, view.near_z}; /* AT near_z, not past it */
    int ax, ay, bx, by;

    TEST_ASSERT_FALSE(r3d_project_segment_cs(p0, p1, &view, &ax, &ay, &bx, &by));
}

/* near_z sits exactly halfway between the two z's, so the crossing point is
 * plain averaging rather than a hand copy of the function's formula. */
static void
test_one_end_behind_clips_to_the_near_plane_crossing(void) {
    r3d_line_view_t view = fixture();
    view.near_z = 100.0F;
    const vec3_t p0 = {0, 0, 0};
    const vec3_t p1 = {200.0F, 100.0F, 200.0F};
    int ax, ay, bx, by;

    TEST_ASSERT_TRUE(r3d_project_segment_cs(p0, p1, &view, &ax, &ay, &bx, &by));

    const vec3_t at_near_z = {(p0.x + p1.x) / 2.0F, (p0.y + p1.y) / 2.0F, view.near_z};
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
    const r3d_line_view_t view = fixture();
    const vec3_t p0 = {50.0F, -30.0F, 300.0F};
    const vec3_t p1 = {-80.0F, 60.0F, 500.0F};
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

/* A point exactly at the camera plane divides by zero; the pixel is a
 * defined, far-off one, never an undefined float-to-int conversion. */
static void
test_a_point_at_the_camera_plane_projects_to_a_far_off_but_defined_pixel(void) {
    const r3d_line_view_t view = fixture();
    const vec3_t p = {1.0F, 1.0F, 0.0F};
    int x = 0, y = 0;

    r3d_camera_to_screen(p, &view, &x, &y);

    TEST_ASSERT_TRUE(x > view.center_x + 10000);
    TEST_ASSERT_TRUE(y < view.center_y - 10000);
}

void
run_r3d_project_suite(void) {
    RUN_TEST(test_to_camera_space_leaves_a_point_unchanged_under_identity);
    RUN_TEST(test_to_camera_space_applies_the_composed_matrix);

    RUN_TEST(test_a_point_on_the_optical_axis_lands_on_center);
    RUN_TEST(test_an_off_axis_point_lands_where_the_formula_says);
    RUN_TEST(test_a_point_exactly_at_near_z_counts_as_behind);
    RUN_TEST(test_orthographic_projection_ignores_depth);
    RUN_TEST(test_a_point_at_the_camera_plane_projects_to_a_far_off_but_defined_pixel);

    RUN_TEST(test_segment_with_both_ends_behind_returns_false);
    RUN_TEST(test_one_end_behind_clips_to_the_near_plane_crossing);
    RUN_TEST(test_both_ends_in_front_matches_projecting_each_point);
}

SUITE_REGISTER(run_r3d_project_suite);

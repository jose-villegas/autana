/*
 * Portable suite: rt_cornell.h/.c - the ray/plane and ray/box primitives, the
 * baked Cornell box scene's shading and shadowing, and the quarter-turn
 * camera. Float results differ in their last bits between x86 and Xtensa, so
 * every assertion here is a tolerance or a colour relationship, never an
 * exact pixel value.
 */

#include <stdbool.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "rt_cornell.h"
#include "rt_geometry.h"
#include "rt_refine.h"

/* Ray/plane */

static void
test_plane_hit_reports_the_correct_distance(void) {
    const rt_plane_t plane = {{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}};
    const r3d_vec3f_t origin = {0.0f, 0.0f, 0.0f};
    const r3d_vec3f_t dir = {0.0f, 0.0f, 1.0f};
    float t;

    TEST_ASSERT_TRUE(rt_intersect_plane(origin, dir, plane, &t));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, t);
}

static void
test_plane_miss_when_the_ray_is_parallel(void) {
    const rt_plane_t plane = {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    const r3d_vec3f_t origin = {0.0f, 0.0f, 0.0f};
    const r3d_vec3f_t dir = {1.0f, 0.0f, 0.0f};
    float t;

    TEST_ASSERT_FALSE(rt_intersect_plane(origin, dir, plane, &t));
}

static void
test_plane_miss_when_the_crossing_is_behind_the_origin(void) {
    const rt_plane_t plane = {{0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, -1.0f}};
    const r3d_vec3f_t origin = {0.0f, 0.0f, 0.0f};
    const r3d_vec3f_t dir = {0.0f, 0.0f, 1.0f};
    float t;

    TEST_ASSERT_FALSE(rt_intersect_plane(origin, dir, plane, &t));
}

/* Ray/box - a 2x2x2 box centred at (0,0,5), unrotated unless a test says so */

static void
test_box_hit_from_outside_reports_the_entry_face(void) {
    const rt_box_t box = {{0.0f, 0.0f, 5.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 1.0f};
    const r3d_vec3f_t origin = {0.0f, 0.0f, 0.0f};
    const r3d_vec3f_t dir = {0.0f, 0.0f, 1.0f};
    float t;
    r3d_vec3f_t n;

    TEST_ASSERT_TRUE(rt_intersect_box(origin, dir, &box, &t, &n));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.0f, t);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.0f, n.z);
}

static void
test_box_miss_when_the_ray_passes_beside_it(void) {
    const rt_box_t box = {{0.0f, 0.0f, 5.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 1.0f};
    const r3d_vec3f_t origin = {5.0f, 0.0f, 0.0f};
    const r3d_vec3f_t dir = {0.0f, 0.0f, 1.0f};
    float t;
    r3d_vec3f_t n;

    TEST_ASSERT_FALSE(rt_intersect_box(origin, dir, &box, &t, &n));
}

static void
test_box_ray_starting_inside_reports_the_exit_face(void) {
    const rt_box_t box = {{0.0f, 0.0f, 5.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 1.0f};
    const r3d_vec3f_t origin = {0.0f, 0.0f, 5.0f}; /* dead centre, inside */
    const r3d_vec3f_t dir = {0.0f, 0.0f, 1.0f};
    float t;
    r3d_vec3f_t n;

    TEST_ASSERT_TRUE(rt_intersect_box(origin, dir, &box, &t, &n));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, t);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, n.z);
}

static void
test_box_ray_parallel_to_a_slab_still_hits_through_the_other_axes(void) {
    const rt_box_t box = {{0.0f, 0.0f, 5.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 1.0f};
    const r3d_vec3f_t origin = {-3.0f, 0.5f, 5.0f}; /* y=0.5 is inside [-1,1]; dir.y == 0 */
    const r3d_vec3f_t dir = {1.0f, 0.0f, 0.0f};
    float t;
    r3d_vec3f_t n;

    TEST_ASSERT_TRUE(rt_intersect_box(origin, dir, &box, &t, &n));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, t); /* -3 + 2 == -1, the box's -X face */
}

static void
test_box_ray_parallel_to_a_slab_and_outside_it_misses(void) {
    const rt_box_t box = {{0.0f, 0.0f, 5.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 1.0f};
    const r3d_vec3f_t origin = {-3.0f, 5.0f, 5.0f}; /* y=5 is outside [-1,1]; dir.y == 0 */
    const r3d_vec3f_t dir = {1.0f, 0.0f, 0.0f};
    float t;
    r3d_vec3f_t n;

    TEST_ASSERT_FALSE(rt_intersect_box(origin, dir, &box, &t, &n));
}

static void
test_a_rotated_box_is_hit_where_the_unrotated_box_would_miss(void) {
    /* A 45-degree box sweeps a corner into the path of a ray that passes
     * just outside the unrotated box's own X extent. */
    const float sin45 = 0.70710678f, cos45 = 0.70710678f;
    const rt_box_t unrotated = {{0.0f, 0.0f, 5.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 1.0f};
    const rt_box_t rotated = {{0.0f, 0.0f, 5.0f}, {1.0f, 1.0f, 1.0f}, sin45, cos45};
    const r3d_vec3f_t origin = {1.3f, 0.0f, 0.0f};
    const r3d_vec3f_t dir = {0.0f, 0.0f, 1.0f};
    float t;
    r3d_vec3f_t n;

    TEST_ASSERT_FALSE(rt_intersect_box(origin, dir, &unrotated, &t, &n));
    TEST_ASSERT_TRUE(rt_intersect_box(origin, dir, &rotated, &t, &n));
}

/* Shadow */

static gfx_color_t
sample_straight_down(float x, float z) {
    rt_cornell_camera_t cam = {
        .origin = {x, 1.9f, z},
        .forward = {0.0f, -1.0f, 0.0f},
        .right = {1.0f, 0.0f, 0.0f},
        .up = {0.0f, 0.0f, 1.0f},
        .half_fov_short_tan = 0.001f,
        .viewport = {.width = 1, .height = 1, .quarter = 0},
    };
    gfx_color_t px;
    rt_cornell_render_row(&cam, 0, &px);
    return px;
}

static int
luminance(gfx_color_t c) {
    const uint32_t rgb = gfx_color_rgb888(c);
    return (int)(((rgb >> 16) & 0xFF) + ((rgb >> 8) & 0xFF) + (rgb & 0xFF));
}

static void
test_a_shadowed_floor_point_is_darker_than_a_lit_one_at_the_same_distance(void) {
    /* (0.75, 0.10) sits behind the short box as seen from the ceiling
     * light; (-0.75, 0.10) mirrors it in X, so both are the same distance
     * from the light with only the box between the first and it. */
    const int shadowed = luminance(sample_straight_down(0.75f, 0.10f));
    const int lit = luminance(sample_straight_down(-0.75f, 0.10f));

    TEST_ASSERT_TRUE_MESSAGE(shadowed < lit - 60, "a floor point behind the short box must be darker than one at the "
                                                  "same distance from the light with a clear view of it");
}

/* Picture sanity */

static void
sample_rgb(int width, int height, int quarter, int x, int y, int* r, int* g, int* b) {
    rt_cornell_camera_t cam;
    rt_cornell_camera_init(&cam, width, height, quarter);
    gfx_color_t* row = malloc(sizeof(*row) * (size_t)width);

    rt_cornell_render_row(&cam, y, row);
    const uint32_t rgb = gfx_color_rgb888(row[x]);
    *r = (int)((rgb >> 16) & 0xFF);
    *g = (int)((rgb >> 8) & 0xFF);
    *b = (int)(rgb & 0xFF);

    free(row);
}

/* A point in the upright picture, as a fraction of the SHORTER axis's half
 * extent from the centre - the room's open front spans about -1..1 on both
 * axes whatever the canvas's shape, so these samples do not move with it. */
static void
room_point(int eff_width, int eff_height, float fx, float fy, int* x, int* y) {
    const float half_short = (float)(eff_width < eff_height ? eff_width : eff_height) * 0.5f;
    *x = (int)((float)eff_width * 0.5f + fx * half_short);
    *y = (int)((float)eff_height * 0.5f - fy * half_short);
}

#define LEFT_WALL_FX  (-0.75f)
#define RIGHT_WALL_FX 0.75f
#define BACK_WALL_FY  0.35f
#define LIGHT_FY      0.67f /* the ceiling light's centre, 3.6 units from the camera */

static void
check_walls_and_back_wall(int width, int height) {
    int r, g, b, x, y;

    room_point(width, height, LEFT_WALL_FX, 0.0f, &x, &y);
    sample_rgb(width, height, 0, x, y, &r, &g, &b);
    TEST_ASSERT_TRUE_MESSAGE(r > g + 30 && r > b + 30, "the left wall must read red-dominant");

    room_point(width, height, RIGHT_WALL_FX, 0.0f, &x, &y);
    sample_rgb(width, height, 0, x, y, &r, &g, &b);
    TEST_ASSERT_TRUE_MESSAGE(g > r + 15 && g > b + 15, "the right wall must read green-dominant");

    room_point(width, height, 0.0f, BACK_WALL_FY, &x, &y);
    sample_rgb(width, height, 0, x, y, &r, &g, &b);
    const int max_c = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const int min_c = r < g ? (r < b ? r : b) : (g < b ? g : b);
    TEST_ASSERT_TRUE_MESSAGE(max_c - min_c < 15, "the back wall must read grey: channels within tolerance");
}

static void
check_light_is_brightest_in_its_row(int width, int height) {
    int r, g, b, x, y;

    room_point(width, height, 0.0f, LIGHT_FY, &x, &y);
    sample_rgb(width, height, 0, x, y, &r, &g, &b);
    const int center_lum = r + g + b;

    room_point(width, height, -0.45f, LIGHT_FY, &x, &y);
    sample_rgb(width, height, 0, x, y, &r, &g, &b);
    const int left_lum = r + g + b;

    room_point(width, height, 0.45f, LIGHT_FY, &x, &y);
    sample_rgb(width, height, 0, x, y, &r, &g, &b);
    const int right_lum = r + g + b;

    TEST_ASSERT_TRUE_MESSAGE(center_lum > left_lum + 100 && center_lum > right_lum + 100,
                             "the light's own centre must be the brightest pixel in its row");
}

static void
test_picture_sanity_at_92x112(void) {
    check_walls_and_back_wall(92, 112);
    check_light_is_brightest_in_its_row(92, 112);
}

static void
test_picture_sanity_at_64x48(void) {
    check_walls_and_back_wall(64, 48);
    check_light_is_brightest_in_its_row(64, 48);
}

static void
test_quarter_1_moves_the_red_wall_to_the_edge_the_roll_says_it_should(void) {
    /* rt_cornell_render_row() draws into the PHYSICAL canvas, pre-rotated so
     * a later read-out at `quarter` (render_host.c's write_bmp(), the same
     * convention ui_transform_quarter_turn() uses) comes out upright. At
     * quarter 1 the upright picture is height x width, and a point on its
     * left wall must still read red at the physical pixel that read-out
     * rotation, run backwards, lands it on. */
    const int width = 92, height = 112;
    int r, g, b, logical_x, logical_y;

    room_point(height, width, LEFT_WALL_FX, 0.0f, &logical_x, &logical_y);
    const int physical_x = width - 1 - logical_y;
    const int physical_y = logical_x;
    sample_rgb(width, height, 1, physical_x, physical_y, &r, &g, &b);
    TEST_ASSERT_TRUE_MESSAGE(r > g + 30 && r > b + 30,
                             "the roll must keep the red wall on the upright image's left edge");

    room_point(height, width, RIGHT_WALL_FX, 0.0f, &logical_x, &logical_y);
    sample_rgb(width, height, 1, width - 1 - logical_y, logical_x, &r, &g, &b);
    TEST_ASSERT_TRUE_MESSAGE(g > r + 15 && g > b + 15, "and the green wall on its right");
}

/* Refinement order */

/* Sizes that are not multiples of the first step, so the lattice's ragged
 * right and bottom edges are part of what is counted. */
static void
check_every_pixel_is_traced_exactly_once(int width, int height) {
    uint8_t* visits = calloc((size_t)width * (size_t)height, 1);
    int passes = 0;

    for (int step = RT_REFINE_FIRST_STEP; step != 0; step = rt_refine_next_step(step)) {
        passes++;
        TEST_ASSERT_EQUAL_INT(passes, rt_refine_pass_number(step));
        for (int y = 0; y < height; y += step) {
            for (int x = 0; x < width; x += step) {
                if (rt_refine_is_new(x, y, step)) {
                    visits[y * width + x]++;
                }
            }
        }
    }

    TEST_ASSERT_EQUAL_INT(RT_REFINE_PASSES, passes);
    for (int i = 0; i < width * height; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, visits[i], "a pixel was skipped or traced twice");
    }
    free(visits);
}

static void
test_refinement_traces_every_pixel_exactly_once(void) {
    check_every_pixel_is_traced_exactly_once(37, 23);
    check_every_pixel_is_traced_exactly_once(16, 9);
    check_every_pixel_is_traced_exactly_once(5, 3);
}

static void
test_first_pass_is_one_pixel_in_sixty_four(void) {
    const int width = 64, height = 48;
    int traced = 0;

    for (int y = 0; y < height; y += RT_REFINE_FIRST_STEP) {
        for (int x = 0; x < width; x += RT_REFINE_FIRST_STEP) {
            traced += rt_refine_is_new(x, y, RT_REFINE_FIRST_STEP) ? 1 : 0;
        }
    }
    TEST_ASSERT_EQUAL_INT(width * height / 64, traced);
}

static void
test_render_pixel_matches_render_row(void) {
    const int width = 40, height = 30;
    rt_cornell_camera_t cam;
    rt_cornell_camera_init(&cam, width, height, 1);

    gfx_color_t row[40];
    rt_cornell_render_row(&cam, 17, row);
    for (int x = 0; x < width; x += 7) {
        TEST_ASSERT_EQUAL_HEX16(row[x], rt_cornell_render_pixel(&cam, x, 17));
    }
}

/* Row canary */

static void
test_render_row_writes_exactly_width_pixels(void) {
    const int width = 40, height = 30;
    rt_cornell_camera_t cam;
    rt_cornell_camera_init(&cam, width, height, 0);

    gfx_color_t buf[41];
    const gfx_color_t canary = (gfx_color_t)0xBEEF;
    buf[width] = canary;

    rt_cornell_render_row(&cam, height / 2, buf);

    TEST_ASSERT_EQUAL_HEX16(canary, buf[width]);
}

void
run_rt_cornell_suite(void) {
    RUN_TEST(test_plane_hit_reports_the_correct_distance);
    RUN_TEST(test_plane_miss_when_the_ray_is_parallel);
    RUN_TEST(test_plane_miss_when_the_crossing_is_behind_the_origin);

    RUN_TEST(test_box_hit_from_outside_reports_the_entry_face);
    RUN_TEST(test_box_miss_when_the_ray_passes_beside_it);
    RUN_TEST(test_box_ray_starting_inside_reports_the_exit_face);
    RUN_TEST(test_box_ray_parallel_to_a_slab_still_hits_through_the_other_axes);
    RUN_TEST(test_box_ray_parallel_to_a_slab_and_outside_it_misses);
    RUN_TEST(test_a_rotated_box_is_hit_where_the_unrotated_box_would_miss);

    RUN_TEST(test_a_shadowed_floor_point_is_darker_than_a_lit_one_at_the_same_distance);

    RUN_TEST(test_picture_sanity_at_92x112);
    RUN_TEST(test_picture_sanity_at_64x48);
    RUN_TEST(test_quarter_1_moves_the_red_wall_to_the_edge_the_roll_says_it_should);

    RUN_TEST(test_refinement_traces_every_pixel_exactly_once);
    RUN_TEST(test_first_pass_is_one_pixel_in_sixty_four);
    RUN_TEST(test_render_pixel_matches_render_row);
    RUN_TEST(test_render_row_writes_exactly_width_pixels);
}

SUITE_REGISTER(run_rt_cornell_suite);

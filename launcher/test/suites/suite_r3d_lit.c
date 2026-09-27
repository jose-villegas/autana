/*
 * Portable suite: r3d_span.h's triangle fill, r3d_lit_pipeline.h's camera,
 * culling and near clip, and r3d_path.h's loop. Every mesh here is built
 * inside the test, never a baked one.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_color.h"
#include "render/r3d_lit_pipeline.h"
#include "render/r3d_path.h"
#include "render/r3d_ray.h"

#define W 64
#define H 48

static gfx_color_t color[W * H];
static uint16_t depth[W * H];

static r3d_span_target_t
fixture(void) {
    memset(color, 0, sizeof color);
    memset(depth, 0, sizeof depth);
    return (r3d_span_target_t){color, depth, W, 0, H};
}

static r3d_span_vertex_t
sv(float x, float y, float z, float r, float g, float b) {
    return (r3d_span_vertex_t){x, y, z, r, g, b};
}

static int
covered(void) {
    int n = 0;
    for (int i = 0; i < W * H; i++) {
        n += depth[i] != 0;
    }
    return n;
}

/* Span fill */

static void
test_two_triangles_sharing_an_edge_cover_a_square_exactly_once(void) {
    const r3d_span_vertex_t a = sv(10.3f, 7.6f, 0.5f, 255, 0, 0), b = sv(40.7f, 9.2f, 0.5f, 255, 0, 0);
    const r3d_span_vertex_t c = sv(37.1f, 41.4f, 0.5f, 255, 0, 0), d = sv(8.9f, 38.8f, 0.5f, 255, 0, 0);
    static uint8_t hits[W * H];
    memset(hits, 0, sizeof hits);

    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &a, &b, &c);
    for (int i = 0; i < W * H; i++) {
        hits[i] += depth[i] != 0;
    }
    t = fixture();
    r3d_span_triangle(&t, &a, &c, &d);
    for (int i = 0; i < W * H; i++) {
        hits[i] += depth[i] != 0;
        TEST_ASSERT_TRUE_MESSAGE(hits[i] <= 1, "a pixel on the shared edge was filled by both triangles");
    }

    /* Nothing inside the quad was missed either: its interior is convex, so
     * a pixel centre strictly between the edges must have been hit once. */
    int filled = 0;
    for (int i = 0; i < W * H; i++) {
        filled += hits[i];
    }
    const float area = 0.5f * fabsf((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y))
                       + 0.5f * fabsf((c.x - a.x) * (d.y - a.y) - (d.x - a.x) * (c.y - a.y));
    TEST_ASSERT_INT_WITHIN((int)(area * 0.03f), (int)area, filled);
}

/* A fan of triangles small enough for the flat path, meeting a large one
 * along a shared edge: every pixel inside is filled once, none twice. */
static void
test_tiny_and_large_triangles_tile_without_gaps_or_overlap(void) {
    static uint8_t hits[W * H];
    memset(hits, 0, sizeof hits);
    const float x0 = 5.3f, y0 = 4.7f, cell = 1.37f;
    int drawn = 0;
    for (int j = 0; j < 12; j++) {
        for (int i = 0; i < 20; i++) {
            const float x = x0 + (float)i * cell, y = y0 + (float)j * cell;
            const r3d_span_vertex_t p00 = sv(x, y, 0.5f, 90, 90, 90), p10 = sv(x + cell, y, 0.5f, 90, 90, 90);
            const r3d_span_vertex_t p01 = sv(x, y + cell, 0.5f, 90, 90, 90);
            const r3d_span_vertex_t p11 = sv(x + cell, y + cell, 0.5f, 90, 90, 90);
            r3d_span_target_t t = fixture();
            r3d_span_triangle(&t, &p00, &p10, &p11);
            r3d_span_triangle(&t, &p00, &p11, &p01);
            for (int k = 0; k < W * H; k++) {
                hits[k] += depth[k] != 0;
            }
            drawn++;
        }
    }
    /* The large triangle shares the fan's bottom edge. */
    const float bottom = y0 + 12.0f * cell, right = x0 + 20.0f * cell;
    const r3d_span_vertex_t la = sv(x0, bottom, 0.5f, 90, 90, 90), lb = sv(right, bottom, 0.5f, 90, 90, 90);
    const r3d_span_vertex_t lc = sv(x0, 46.0f, 0.5f, 90, 90, 90);
    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &la, &lb, &lc);
    for (int k = 0; k < W * H; k++) {
        hits[k] += depth[k] != 0;
        TEST_ASSERT_TRUE_MESSAGE(hits[k] <= 1, "a pixel was filled by two triangles");
    }
    for (int y = (int)ceilf(y0 + 0.5f); y + 0.5f < bottom - 0.5f; y++) {
        for (int x = (int)ceilf(x0 + 0.5f); x + 0.5f < right - 0.5f; x++) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, hits[y * W + x], "a pixel inside the fan was missed");
        }
    }
    TEST_ASSERT_EQUAL_INT(240, drawn);
}

static void
test_the_nearer_triangle_wins_in_either_order(void) {
    const r3d_span_vertex_t fa = sv(0, 0, 0.2f, 255, 0, 0), fb = sv(60, 0, 0.2f, 255, 0, 0),
                            fc = sv(0, 45, 0.2f, 255, 0, 0);
    const r3d_span_vertex_t na = sv(0, 0, 0.8f, 0, 0, 255), nb = sv(60, 0, 0.8f, 0, 0, 255),
                            nc = sv(0, 45, 0.8f, 0, 0, 255);

    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &fa, &fb, &fc);
    r3d_span_triangle(&t, &na, &nb, &nc);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x0000FF), color[5 * W + 5]);

    t = fixture();
    r3d_span_triangle(&t, &na, &nb, &nc);
    r3d_span_triangle(&t, &fa, &fb, &fc);
    TEST_ASSERT_EQUAL_HEX16(GFX_RGB(0x0000FF), color[5 * W + 5]);
}

static void
test_a_window_of_rows_matches_the_same_rows_of_a_full_draw(void) {
    const r3d_span_vertex_t a = sv(3.2f, -20.0f, 0.3f, 10, 200, 30), b = sv(70.0f, 12.5f, 0.9f, 250, 20, 90);
    const r3d_span_vertex_t c = sv(-5.0f, 60.0f, 0.6f, 90, 90, 250);

    r3d_span_target_t full = fixture();
    r3d_span_triangle(&full, &a, &b, &c);
    static gfx_color_t whole[W * H];
    memcpy(whole, color, sizeof whole);

    static gfx_color_t band_color[W * 16];
    static uint16_t band_depth[W * 16];
    memset(band_color, 0, sizeof band_color);
    memset(band_depth, 0, sizeof band_depth);
    const r3d_span_target_t band = {band_color, band_depth, W, 16, 32};
    r3d_span_triangle(&band, &a, &b, &c);
    TEST_ASSERT_EQUAL_MEMORY(whole + 16 * W, band_color, sizeof band_color);
}

static void
test_colours_stay_within_the_vertex_range_even_at_the_edges(void) {
    const r3d_span_vertex_t a = sv(0.4f, 0.4f, 0.5f, 255, 255, 255), b = sv(63.6f, 1.0f, 0.5f, 255, 255, 255);
    const r3d_span_vertex_t c = sv(2.0f, 47.6f, 0.5f, 0, 0, 0);
    r3d_span_target_t t = fixture();
    r3d_span_triangle(&t, &a, &b, &c);
    for (int i = 0; i < W * H; i++) {
        if (depth[i] == 0) {
            continue;
        }
        const uint16_t native = (uint16_t)((color[i] >> 8) | (color[i] << 8));
        const int r = native >> 11, g = (native >> 5) & 63, b5 = native & 31;
        /* A wrapped channel would show as a colour whose channels disagree. */
        TEST_ASSERT_INT_WITHIN(1, r, b5);
        TEST_ASSERT_INT_WITHIN(2, r * 2, g);
    }
}

/* Camera and pipeline */

static const int16_t quad_positions[][3] = {{-100, -100, 0}, {100, -100, 0}, {100, 100, 0}, {-100, 100, 0}};
static const uint8_t quad_colors[][3] = {{255, 255, 255}, {255, 255, 255}, {255, 255, 255}, {255, 255, 255}};
static const uint16_t quad_front[][3] = {{0, 1, 2}, {0, 2, 3}};
static const uint16_t quad_back[][3] = {{0, 2, 1}, {0, 3, 2}};

static const r3d_lit_node_t quad_node = {{-100, -100, 0}, {100, 100, 0}, 0, 1, true};

static r3d_lit_mesh_t
quad_mesh(const uint16_t (*triangles)[3], bool double_sided, r3d_lit_cluster_t* cluster) {
    *cluster = (r3d_lit_cluster_t){0, 4, 0, 2, {-100, -100, 0}, {100, 100, 0}, double_sided};
    return (r3d_lit_mesh_t){quad_positions, quad_colors, triangles, cluster, &quad_node, 4, 2, 1, 1, 1};
}

/* The quad faces +z; this camera stands on +z looking back at it. */
static int
draw_quad(const uint16_t (*triangles)[3], bool double_sided) {
    r3d_lit_cluster_t cluster;
    const r3d_lit_mesh_t mesh = quad_mesh(triangles, double_sided, &cluster);
    r3d_lit_view_t view;
    r3d_lit_view_look(&view, (r3d_vec3f_t){0, 0, 400}, (r3d_vec3f_t){0, 0, -1}, 0.5f, 1.0f, 1,
                      (r3d_viewport_t){W, H, 0});
    uint16_t visible[1];
    const int count = r3d_lit_cull_clusters(&mesh, &view, visible);
    r3d_lit_vertex_t cs[4];
    r3d_lit_transform(&mesh, &view, visible, count, cs, NULL);
    const r3d_span_target_t t = fixture();
    r3d_lit_draw(&mesh, &view, visible, count, cs, NULL, &t);
    return covered();
}

static void
test_a_counter_clockwise_face_toward_the_camera_is_drawn(void) {
    TEST_ASSERT_GREATER_THAN_INT(0, draw_quad(quad_front, false));
}

static void
test_a_face_turned_away_is_culled_unless_double_sided(void) {
    TEST_ASSERT_EQUAL_INT(0, draw_quad(quad_back, false));
    TEST_ASSERT_EQUAL_INT(draw_quad(quad_front, false), draw_quad(quad_back, true));
}

static void
test_a_cluster_behind_the_camera_is_culled(void) {
    r3d_lit_cluster_t cluster;
    const r3d_lit_mesh_t mesh = quad_mesh(quad_front, false, &cluster);
    r3d_lit_view_t view;
    r3d_lit_view_look(&view, (r3d_vec3f_t){0, 0, 400}, (r3d_vec3f_t){0, 0, 1}, 0.5f, 1.0f, 1,
                      (r3d_viewport_t){W, H, 0});
    uint16_t visible[1];
    TEST_ASSERT_EQUAL_INT(0, r3d_lit_cull_clusters(&mesh, &view, visible));
}

/* A floor running from behind the camera to far ahead: the near clip keeps
 * the part in front and nothing lands above the horizon row. */
static void
test_a_floor_crossing_the_near_plane_draws_only_below_the_horizon(void) {
    static const int16_t floor_positions[][3] = {
        {-1000, 0, 1000}, {1000, 0, 1000}, {1000, 0, -3000}, {-1000, 0, -3000}};
    static const uint16_t floor_up[][3] = {{0, 1, 2}, {0, 2, 3}};
    r3d_lit_cluster_t cluster = {0, 4, 0, 2, {-1000, 0, -3000}, {1000, 0, 1000}, false};
    static const r3d_lit_node_t floor_node = {{-1000, 0, -3000}, {1000, 0, 1000}, 0, 1, true};
    const r3d_lit_mesh_t mesh = {floor_positions, quad_colors, floor_up, &cluster, &floor_node, 4, 2, 1, 1, 1};

    r3d_lit_view_t view;
    r3d_lit_view_look(&view, (r3d_vec3f_t){0, 50, 0}, (r3d_vec3f_t){0, 0, -1}, 0.5f, 1.0f, 1,
                      (r3d_viewport_t){W, H, 0});
    uint16_t visible[1];
    const int count = r3d_lit_cull_clusters(&mesh, &view, visible);
    TEST_ASSERT_EQUAL_INT(1, count);
    r3d_lit_vertex_t cs[4];
    r3d_lit_transform(&mesh, &view, visible, count, cs, NULL);
    const r3d_span_target_t t = fixture();
    r3d_lit_draw(&mesh, &view, visible, count, cs, NULL, &t);

    for (int y = 0; y < H / 2; y++) {
        for (int x = 0; x < W; x++) {
            TEST_ASSERT_EQUAL_UINT16(0, depth[y * W + x]);
        }
    }
    for (int x = 0; x < W; x++) {
        TEST_ASSERT_NOT_EQUAL(0, depth[(H - 1) * W + x]);
    }
}

/* A point up and to the right of the view axis lands up and to the right in
 * the upright picture, whichever way the panel is turned. */
static void
test_a_point_up_and_right_lands_up_and_right_in_every_quarter(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        const r3d_viewport_t viewport = {W, H, quarter};
        r3d_lit_view_t view;
        r3d_lit_view_look(&view, (r3d_vec3f_t){0, 0, 0}, (r3d_vec3f_t){0, 0, -1}, 0.5f, 1.0f, 1, viewport);
        const float px = view.m[0][0] * 30 + view.m[0][1] * 20 + view.m[0][2] * -100 + view.m[0][3];
        const float py = view.m[1][0] * 30 + view.m[1][1] * 20 + view.m[1][2] * -100 + view.m[1][3];
        const float pz = view.m[2][0] * 30 + view.m[2][1] * 20 + view.m[2][2] * -100 + view.m[2][3];
        int ux, uy;
        r3d_physical_to_upright(viewport, (int)floorf(view.center_x + px / pz), (int)floorf(view.center_y + py / pz),
                                &ux, &uy);

        /* The lens fits the shorter upright axis: tan = 0.5 spans half of it. */
        const int upright_width = (quarter & 1) ? H : W, upright_height = (quarter & 1) ? W : H;
        const float per_unit = (float)(W < H ? W : H) / (2.0f * 0.5f) / 100.0f;
        TEST_ASSERT_INT_WITHIN(1, (int)((float)upright_width / 2.0f + 30.0f * per_unit), ux);
        TEST_ASSERT_INT_WITHIN(1, (int)((float)upright_height / 2.0f - 20.0f * per_unit), uy);
    }
}

/* Camera path */

static const r3d_waypoint_t square[] = {
    {{0, 0, 0}, {0, 0, -1}},
    {{100, 0, 0}, {100, 0, -1}},
    {{100, 0, 100}, {100, 0, 99}},
    {{0, 0, 100}, {0, 0, 99}},
};
static const r3d_path_t square_path = {square, 4, 50.0f, 1.0f};

static void
test_the_path_passes_through_each_waypoint(void) {
    uint32_t t = 0;
    for (int i = 0; i < 4; i++) {
        r3d_vec3f_t eye, forward;
        r3d_path_sample(&square_path, t, &eye, &forward);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, square[i].eye.x, eye.x);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, square[i].eye.z, eye.z);
        TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.0f, forward.z);
        t += 2000; /* 100 units at 50 per second */
    }
    TEST_ASSERT_EQUAL_UINT32(8000, r3d_path_period_ms(&square_path));
}

static void
test_the_path_is_continuous_and_loops(void) {
    const uint32_t period = r3d_path_period_ms(&square_path);
    r3d_vec3f_t prev, forward;
    r3d_path_sample(&square_path, 0, &prev, &forward);
    for (uint32_t t = 10; t <= period; t += 10) {
        r3d_vec3f_t eye;
        r3d_path_sample(&square_path, t, &eye, &forward);
        const float step = sqrtf((eye.x - prev.x) * (eye.x - prev.x) + (eye.z - prev.z) * (eye.z - prev.z));
        TEST_ASSERT_TRUE_MESSAGE(step < 1.5f, "the eye jumped between two samples 10 ms apart");
        prev = eye;
    }
}

static void
run_r3d_lit_suite(void) {
    RUN_TEST(test_two_triangles_sharing_an_edge_cover_a_square_exactly_once);
    RUN_TEST(test_tiny_and_large_triangles_tile_without_gaps_or_overlap);
    RUN_TEST(test_the_nearer_triangle_wins_in_either_order);
    RUN_TEST(test_a_window_of_rows_matches_the_same_rows_of_a_full_draw);
    RUN_TEST(test_colours_stay_within_the_vertex_range_even_at_the_edges);

    RUN_TEST(test_a_counter_clockwise_face_toward_the_camera_is_drawn);
    RUN_TEST(test_a_face_turned_away_is_culled_unless_double_sided);
    RUN_TEST(test_a_cluster_behind_the_camera_is_culled);
    RUN_TEST(test_a_floor_crossing_the_near_plane_draws_only_below_the_horizon);
    RUN_TEST(test_a_point_up_and_right_lands_up_and_right_in_every_quarter);

    RUN_TEST(test_the_path_passes_through_each_waypoint);
    RUN_TEST(test_the_path_is_continuous_and_loops);
}

SUITE_REGISTER(run_r3d_lit_suite);

/*
 * Portable suite: wire_pipeline.h/.c - the per-frame transform and the
 * per-edge near/screen clip that turn a wire_mesh_t into wire_segment_t's a
 * scene hands to gfx_line(). Every mesh here is built inside the test, never
 * the baked ones in wire_primitives_generated.h.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "wire_pipeline.h"

#define SCREEN_W 368
#define SCREEN_H 448

static r3d_view_t
fixture(void) {
    r3d_view_t v;
    S3L_mat4Init(v.matrix);
    v.focal = S3L_F;
    v.near_z = R3D_NEAR_Z;
    v.center_x = SCREEN_W / 2;
    v.center_y = SCREEN_H / 2;
    v.scale = SCREEN_W / 2;
    return v;
}

/* Known pose, exact expected segments */

static const wire_vertex_t identity_pose_vertices[2] = {
    {0, 0, 5 * S3L_F},
    {2 * S3L_F, -4 * S3L_F, 4 * S3L_F},
};
static const wire_edge_t one_edge[1] = {{0, 1}};
static const wire_mesh_t identity_pose_mesh = {identity_pose_vertices, one_edge, 2, 1};

static void
test_identity_pose_matches_the_hand_derived_screen_points(void) {
    const r3d_view_t view = fixture();
    wire_cs_vertex_t cs[2];
    wire_segment_t segments[1];
    wire_frame_t frame = {cs, 2, segments, 1, 0, 0, 0, 0, 0};

    wire_transform(&identity_pose_mesh, &view, &frame);
    TEST_ASSERT_TRUE(wire_project_edges(&identity_pose_mesh, &view, SCREEN_W, SCREEN_H, &frame));

    TEST_ASSERT_EQUAL_INT(1, frame.segment_count);
    TEST_ASSERT_EQUAL_INT(184, segments[0].x0);
    TEST_ASSERT_EQUAL_INT(224, segments[0].y0);
    TEST_ASSERT_EQUAL_INT(276, segments[0].x1);
    TEST_ASSERT_EQUAL_INT(408, segments[0].y1);
}

/* A 90-degree rotation about Y, built by hand (x'=z, y'=y, z'=x) rather than
 * through the trig table, so the expected output has no rounding of its
 * own to account for. */
static const wire_vertex_t rotated_pose_vertices[2] = {
    {5 * S3L_F, 0, 0},
    {2 * S3L_F, -4 * S3L_F, 4 * S3L_F},
};
static const wire_mesh_t rotated_pose_mesh = {rotated_pose_vertices, one_edge, 2, 1};

static void
test_a_rotated_pose_matches_the_hand_derived_screen_points(void) {
    r3d_view_t view = fixture();
    S3L_mat4Init(view.matrix);
    view.matrix[0][0] = 0;
    view.matrix[0][2] = S3L_F;
    view.matrix[2][0] = S3L_F;
    view.matrix[2][2] = 0;
    view.center_x = 50;
    view.center_y = 80;
    view.scale = 40;
    view.focal = 2 * S3L_F;
    wire_cs_vertex_t cs[2];
    wire_segment_t segments[1];
    wire_frame_t frame = {cs, 2, segments, 1, 0, 0, 0, 0, 0};

    wire_transform(&rotated_pose_mesh, &view, &frame);
    TEST_ASSERT_TRUE(wire_project_edges(&rotated_pose_mesh, &view, SCREEN_W, SCREEN_H, &frame));

    TEST_ASSERT_EQUAL_INT(1, frame.segment_count);
    TEST_ASSERT_EQUAL_INT(50, segments[0].x0);
    TEST_ASSERT_EQUAL_INT(80, segments[0].y0);
    TEST_ASSERT_EQUAL_INT(210, segments[0].x1);
    TEST_ASSERT_EQUAL_INT(240, segments[0].y1);
}

/* Plane grids: shared vertices transformed once, full visibility */

static int
build_grid(int n, wire_vertex_t* vertices, wire_edge_t* edges) {
    const int step = S3L_F;
    const int base_z = 20 * S3L_F;
    const int half = (n - 1) * step / 2;

    for (int row = 0; row < n; row++) {
        for (int col = 0; col < n; col++) {
            vertices[row * n + col].x = (int16_t)(col * step - half);
            vertices[row * n + col].y = 0;
            vertices[row * n + col].z = (int16_t)(base_z + row * step - half);
        }
    }

    int edge_count = 0;
    for (int row = 0; row < n; row++) {
        for (int col = 0; col < n - 1; col++) {
            edges[edge_count].a = (uint16_t)(row * n + col);
            edges[edge_count].b = (uint16_t)(row * n + col + 1);
            edge_count++;
        }
    }
    for (int col = 0; col < n; col++) {
        for (int row = 0; row < n - 1; row++) {
            edges[edge_count].a = (uint16_t)(row * n + col);
            edges[edge_count].b = (uint16_t)((row + 1) * n + col);
            edge_count++;
        }
    }
    return edge_count;
}

/* Heap-allocated rather than a stack array sized for the largest grid
 * this suite tests: the device's main task stack is 3,584 bytes total,
 * shared with Unity and printf, and a vertex/edge/cs/segment set for that
 * size already crowds the frame-size ceiling in one function. */
static void
check_grid_is_fully_visible(int n) {
    const r3d_view_t view = fixture();
    const int vertex_count = n * n;
    const int max_edges = 2 * n * (n - 1);
    wire_vertex_t* vertices = malloc(sizeof(*vertices) * (size_t)vertex_count);
    wire_edge_t* edges = malloc(sizeof(*edges) * (size_t)max_edges);
    wire_cs_vertex_t* cs = malloc(sizeof(*cs) * (size_t)(vertex_count + 1));
    wire_segment_t* segments = malloc(sizeof(*segments) * (size_t)max_edges);

    const int edge_count = build_grid(n, vertices, edges);
    const wire_mesh_t mesh = {vertices, edges, (uint16_t)vertex_count, (uint16_t)edge_count};

    const int32_t canary = -12345;
    cs[vertex_count].x = canary;
    cs[vertex_count].y = canary;
    cs[vertex_count].z = canary;
    wire_frame_t frame = {cs, (uint16_t)vertex_count, segments, (uint16_t)edge_count, 0, 0, 0, 0, 0};

    wire_transform(&mesh, &view, &frame);
    TEST_ASSERT_EQUAL_INT32(canary, cs[vertex_count].x);
    TEST_ASSERT_EQUAL_INT32(canary, cs[vertex_count].y);
    TEST_ASSERT_EQUAL_INT32(canary, cs[vertex_count].z);

    TEST_ASSERT_TRUE(wire_project_edges(&mesh, &view, SCREEN_W, SCREEN_H, &frame));
    TEST_ASSERT_EQUAL_INT(edge_count, frame.segment_count);

    free(vertices);
    free(edges);
    free(cs);
    free(segments);
}

static void
test_a_2x2_grid_is_fully_visible(void) {
    check_grid_is_fully_visible(2);
}

static void
test_a_3x3_grid_is_fully_visible(void) {
    check_grid_is_fully_visible(3);
}

static void
test_a_5x5_grid_is_fully_visible(void) {
    check_grid_is_fully_visible(5);
}

/* Near-plane clip */

static void
test_both_ends_behind_near_plane_drop_the_edge(void) {
    const r3d_view_t view = fixture();
    const wire_vertex_t vertices[2] = {{0, 0, 0}, {10, 10, (int16_t)view.near_z}};
    const wire_mesh_t mesh = {vertices, one_edge, 2, 1};
    wire_cs_vertex_t cs[2];
    wire_segment_t segments[1];
    wire_frame_t frame = {cs, 2, segments, 1, 0, 0, 0, 0, 0};

    wire_transform(&mesh, &view, &frame);
    TEST_ASSERT_TRUE(wire_project_edges(&mesh, &view, SCREEN_W, SCREEN_H, &frame));
    TEST_ASSERT_EQUAL_INT(0, frame.segment_count);
}

static void
test_one_end_behind_clips_to_the_near_plane_crossing(void) {
    r3d_view_t view = fixture();
    view.near_z = 100;
    const wire_vertex_t vertices[2] = {{0, 0, 0}, {40, 20, 200}};
    const wire_mesh_t mesh = {vertices, one_edge, 2, 1};
    wire_cs_vertex_t cs[2];
    wire_segment_t segments[1];
    wire_frame_t frame = {cs, 2, segments, 1, 0, 0, 0, 0, 0};

    wire_transform(&mesh, &view, &frame);
    TEST_ASSERT_TRUE(wire_project_edges(&mesh, &view, SCREEN_W, SCREEN_H, &frame));
    TEST_ASSERT_EQUAL_INT(1, frame.segment_count);

    /* near_z sits exactly halfway between the two z's, so the crossing is
     * plain averaging, independent of the pipeline's own Q16 interpolation. */
    const S3L_Vec4 crossing = {20, 10, view.near_z, S3L_F};
    const S3L_Vec4 front = {40, 20, 200, S3L_F};
    int ex, ey, fx, fy;
    r3d_camera_to_screen(crossing, &view, &ex, &ey);
    r3d_camera_to_screen(front, &view, &fx, &fy);

    TEST_ASSERT_EQUAL_INT(ex, segments[0].x0);
    TEST_ASSERT_EQUAL_INT(ey, segments[0].y0);
    TEST_ASSERT_EQUAL_INT(fx, segments[0].x1);
    TEST_ASSERT_EQUAL_INT(fy, segments[0].y1);
}

static void
test_an_endpoint_exactly_at_near_z_counts_as_behind(void) {
    const r3d_view_t view = fixture();
    const wire_vertex_t vertices[2] = {{10, 10, (int16_t)view.near_z}, {0, 0, 300}};
    const wire_mesh_t mesh = {vertices, one_edge, 2, 1};
    wire_cs_vertex_t cs[2];
    wire_segment_t segments[1];
    wire_frame_t frame = {cs, 2, segments, 1, 0, 0, 0, 0, 0};

    wire_transform(&mesh, &view, &frame);
    TEST_ASSERT_TRUE(wire_project_edges(&mesh, &view, SCREEN_W, SCREEN_H, &frame));
    TEST_ASSERT_EQUAL_INT(1, frame.segment_count);

    const S3L_Vec4 p0 = {10, 10, view.near_z, S3L_F};
    const S3L_Vec4 p1 = {0, 0, 300, S3L_F};
    int ex, ey, fx, fy;
    r3d_camera_to_screen(p0, &view, &ex, &ey);
    r3d_camera_to_screen(p1, &view, &fx, &fy);

    TEST_ASSERT_EQUAL_INT(ex, segments[0].x0);
    TEST_ASSERT_EQUAL_INT(ey, segments[0].y0);
    TEST_ASSERT_EQUAL_INT(fx, segments[0].x1);
    TEST_ASSERT_EQUAL_INT(fy, segments[0].y1);
}

/* Off-screen rejection and the screen-edge clip */

static void
check_edge_dropped(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    const r3d_view_t view = fixture();
    const wire_vertex_t vertices[2] = {{x0, y0, S3L_F}, {x1, y1, S3L_F}};
    const wire_mesh_t mesh = {vertices, one_edge, 2, 1};
    wire_cs_vertex_t cs[2];
    wire_segment_t segments[1];
    wire_frame_t frame = {cs, 2, segments, 1, 0, 0, 0, 0, 0};

    wire_transform(&mesh, &view, &frame);
    TEST_ASSERT_TRUE(wire_project_edges(&mesh, &view, SCREEN_W, SCREEN_H, &frame));
    TEST_ASSERT_EQUAL_INT(0, frame.segment_count);
}

static void
test_an_edge_fully_left_of_the_screen_is_dropped(void) {
    check_edge_dropped(-3 * S3L_F, 0, -2 * S3L_F, 0);
}

static void
test_an_edge_fully_right_of_the_screen_is_dropped(void) {
    check_edge_dropped(3 * S3L_F, 0, 4 * S3L_F, 0);
}

static void
test_an_edge_fully_above_the_screen_is_dropped(void) {
    check_edge_dropped(0, 2 * S3L_F, 0, 3 * S3L_F);
}

static void
test_an_edge_fully_below_the_screen_is_dropped(void) {
    check_edge_dropped(0, -2 * S3L_F, 0, -3 * S3L_F);
}

static void
test_an_edge_straddling_the_right_edge_is_kept_and_clipped(void) {
    const r3d_view_t view = fixture();
    const wire_vertex_t vertices[2] = {{0, 0, S3L_F}, {4 * S3L_F, 0, S3L_F}};
    const wire_mesh_t mesh = {vertices, one_edge, 2, 1};
    wire_cs_vertex_t cs[2];
    wire_segment_t segments[1];
    wire_frame_t frame = {cs, 2, segments, 1, 0, 0, 0, 0, 0};

    wire_transform(&mesh, &view, &frame);
    TEST_ASSERT_TRUE(wire_project_edges(&mesh, &view, SCREEN_W, SCREEN_H, &frame));

    TEST_ASSERT_EQUAL_INT(1, frame.segment_count);
    TEST_ASSERT_EQUAL_INT(184, segments[0].x0);
    TEST_ASSERT_EQUAL_INT(224, segments[0].y0);
    TEST_ASSERT_EQUAL_INT(SCREEN_W - 1, segments[0].x1);
    TEST_ASSERT_EQUAL_INT(224, segments[0].y1);
}

/* A large model*view scale (standing in for a near-camera point, which
 * amplifies the same way through perspective divide) sends camera space far
 * past int16 range while the model vertices themselves stay int16-safe,
 * exercising the 64-bit clip that runs before narrowing. */
static void
test_a_far_off_endpoint_is_narrowed_within_one_pixel(void) {
    r3d_view_t view = fixture();
    S3L_mat4Init(view.matrix);
    view.matrix[0][0] = 500 * S3L_F;
    view.matrix[1][1] = 250 * S3L_F;

    const wire_vertex_t vertices[2] = {{0, 0, 5 * S3L_F}, {S3L_F, S3L_F, 52}};
    const wire_mesh_t mesh = {vertices, one_edge, 2, 1};

    const S3L_Vec4 model_far = {500 * S3L_F, 250 * S3L_F, 52, S3L_F};
    const S3L_Vec4 model_near = {0, 0, 5 * S3L_F, S3L_F};
    int fx_screen, fy_screen, nx_screen, ny_screen;
    r3d_camera_to_screen(model_far, &view, &fx_screen, &fy_screen);
    r3d_camera_to_screen(model_near, &view, &nx_screen, &ny_screen);

    wire_cs_vertex_t cs[2];
    wire_segment_t segments[1];
    wire_frame_t frame = {cs, 2, segments, 1, 0, 0, 0, 0, 0};

    wire_transform(&mesh, &view, &frame);
    TEST_ASSERT_TRUE(wire_project_edges(&mesh, &view, SCREEN_W, SCREEN_H, &frame));
    TEST_ASSERT_EQUAL_INT(1, frame.segment_count);
    TEST_ASSERT_EQUAL_INT(nx_screen, segments[0].x0);
    TEST_ASSERT_EQUAL_INT(ny_screen, segments[0].y0);
    TEST_ASSERT_EQUAL_INT(SCREEN_W - 1, segments[0].x1);

    /* Ideal (unrounded) y where the raw line crosses x = SCREEN_W - 1,
     * checked by cross-multiplication so no float or second truncation is
     * needed to bound the error. */
    const int64_t dx = fx_screen - nx_screen;
    const int64_t dy = fy_screen - ny_screen;
    const int64_t num = (int64_t)ny_screen * dx + dy * (SCREEN_W - 1 - nx_screen);
    const int64_t diff = (int64_t)segments[0].y1 * dx - num;
    const int64_t bound = dx < 0 ? -dx : dx;
    TEST_ASSERT_TRUE(diff <= bound && diff >= -bound);
}

/* Capacity */

static void
test_exceeding_capacity_caps_the_count_and_reports_overflow(void) {
    const r3d_view_t view = fixture();
    const int n = 2;
    const int vertex_count = n * n;
    wire_vertex_t vertices[4];
    wire_edge_t edges[4];
    const int edge_count = build_grid(n, vertices, edges);
    const wire_mesh_t mesh = {vertices, edges, (uint16_t)vertex_count, (uint16_t)edge_count};

    wire_cs_vertex_t cs[4];
    wire_segment_t segments[3];
    const int16_t canary = -777;
    segments[2].x0 = canary;
    segments[2].y0 = canary;
    segments[2].x1 = canary;
    segments[2].y1 = canary;
    wire_frame_t frame = {cs, (uint16_t)vertex_count, segments, 2, 0, 0, 0, 0, 0};

    wire_transform(&mesh, &view, &frame);
    TEST_ASSERT_FALSE(wire_project_edges(&mesh, &view, SCREEN_W, SCREEN_H, &frame));

    TEST_ASSERT_EQUAL_INT(2, frame.segment_count);
    TEST_ASSERT_EQUAL_INT16(canary, segments[2].x0);
    TEST_ASSERT_EQUAL_INT16(canary, segments[2].y0);
    TEST_ASSERT_EQUAL_INT16(canary, segments[2].x1);
    TEST_ASSERT_EQUAL_INT16(canary, segments[2].y1);
}

/* Bounding box */

static const wire_vertex_t bbox_vertices[4] = {
    {0, 0, 5 * S3L_F},
    {2 * S3L_F, 0, 5 * S3L_F},
    {0, 2 * S3L_F, 5 * S3L_F},
    {-2 * S3L_F, -2 * S3L_F, 5 * S3L_F},
};
static const wire_edge_t bbox_edges[2] = {{0, 1}, {2, 3}};
static const wire_mesh_t bbox_mesh = {bbox_vertices, bbox_edges, 4, 2};

static void
test_bbox_is_the_union_of_the_emitted_segments(void) {
    const r3d_view_t view = fixture();
    wire_cs_vertex_t cs[4];
    wire_segment_t segments[2];
    wire_frame_t frame = {cs, 4, segments, 2, 0, 0, 0, 0, 0};

    wire_transform(&bbox_mesh, &view, &frame);
    TEST_ASSERT_TRUE(wire_project_edges(&bbox_mesh, &view, SCREEN_W, SCREEN_H, &frame));
    TEST_ASSERT_EQUAL_INT(2, frame.segment_count);

    int expect_x0 = segments[0].x0, expect_x1 = segments[0].x0;
    int expect_y0 = segments[0].y0, expect_y1 = segments[0].y0;
    for (int i = 0; i < frame.segment_count; i++) {
        const int xs[2] = {segments[i].x0, segments[i].x1};
        const int ys[2] = {segments[i].y0, segments[i].y1};
        for (int j = 0; j < 2; j++) {
            if (xs[j] < expect_x0) {
                expect_x0 = xs[j];
            }
            if (xs[j] > expect_x1) {
                expect_x1 = xs[j];
            }
            if (ys[j] < expect_y0) {
                expect_y0 = ys[j];
            }
            if (ys[j] > expect_y1) {
                expect_y1 = ys[j];
            }
        }
    }

    TEST_ASSERT_EQUAL_INT(expect_x0, frame.bbox_x0);
    TEST_ASSERT_EQUAL_INT(expect_y0, frame.bbox_y0);
    TEST_ASSERT_EQUAL_INT(expect_x1 + 1, frame.bbox_x1);
    TEST_ASSERT_EQUAL_INT(expect_y1 + 1, frame.bbox_y1);
}

/* wire_segment_overlaps_rows */

static void
test_overlaps_rows_horizontal_segment(void) {
    const wire_segment_t seg = {0, 50, 100, 50};
    TEST_ASSERT_TRUE(wire_segment_overlaps_rows(&seg, 40, 60));
    TEST_ASSERT_FALSE(wire_segment_overlaps_rows(&seg, 51, 60));
}

static void
test_overlaps_rows_vertical_segment(void) {
    const wire_segment_t seg = {10, 20, 10, 80};
    TEST_ASSERT_TRUE(wire_segment_overlaps_rows(&seg, 0, 30));
    TEST_ASSERT_TRUE(wire_segment_overlaps_rows(&seg, 70, 90));
    TEST_ASSERT_FALSE(wire_segment_overlaps_rows(&seg, 81, 90));
}

static void
test_overlaps_rows_ignores_endpoint_order(void) {
    const wire_segment_t forward = {0, 20, 0, 80};
    const wire_segment_t reversed = {0, 80, 0, 20};
    TEST_ASSERT_EQUAL_INT(wire_segment_overlaps_rows(&forward, 30, 40), wire_segment_overlaps_rows(&reversed, 30, 40));
    TEST_ASSERT_TRUE(wire_segment_overlaps_rows(&reversed, 30, 40));
}

/* Only the row RANGE is half-open; a segment's own endpoint is a row it
 * genuinely occupies, so a segment lying on row 20 overlaps a band that
 * starts at 20 and misses one that ends there. */
static void
test_overlaps_rows_boundary_row_is_half_open(void) {
    const wire_segment_t seg = {0, 20, 50, 20};
    TEST_ASSERT_TRUE(wire_segment_overlaps_rows(&seg, 20, 21));
    TEST_ASSERT_FALSE(wire_segment_overlaps_rows(&seg, 10, 20));
    TEST_ASSERT_TRUE(wire_segment_overlaps_rows(&seg, 19, 21));
    TEST_ASSERT_FALSE(wire_segment_overlaps_rows(&seg, 21, 30));
}

void
run_wire_pipeline_suite(void) {
    RUN_TEST(test_identity_pose_matches_the_hand_derived_screen_points);
    RUN_TEST(test_a_rotated_pose_matches_the_hand_derived_screen_points);

    RUN_TEST(test_a_2x2_grid_is_fully_visible);
    RUN_TEST(test_a_3x3_grid_is_fully_visible);
    RUN_TEST(test_a_5x5_grid_is_fully_visible);

    RUN_TEST(test_both_ends_behind_near_plane_drop_the_edge);
    RUN_TEST(test_one_end_behind_clips_to_the_near_plane_crossing);
    RUN_TEST(test_an_endpoint_exactly_at_near_z_counts_as_behind);

    RUN_TEST(test_an_edge_fully_left_of_the_screen_is_dropped);
    RUN_TEST(test_an_edge_fully_right_of_the_screen_is_dropped);
    RUN_TEST(test_an_edge_fully_above_the_screen_is_dropped);
    RUN_TEST(test_an_edge_fully_below_the_screen_is_dropped);
    RUN_TEST(test_an_edge_straddling_the_right_edge_is_kept_and_clipped);
    RUN_TEST(test_a_far_off_endpoint_is_narrowed_within_one_pixel);

    RUN_TEST(test_exceeding_capacity_caps_the_count_and_reports_overflow);

    RUN_TEST(test_bbox_is_the_union_of_the_emitted_segments);

    RUN_TEST(test_overlaps_rows_horizontal_segment);
    RUN_TEST(test_overlaps_rows_vertical_segment);
    RUN_TEST(test_overlaps_rows_ignores_endpoint_order);
    RUN_TEST(test_overlaps_rows_boundary_row_is_half_open);
}

SUITE_REGISTER(run_wire_pipeline_suite);

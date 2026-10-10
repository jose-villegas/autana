/*
 * Host-only suite: tools/r3d/triangle_sizes.h, the triangle-size histogram,
 * the box histogram and the poses file, on a mesh built inside the test. No
 * firmware image compiles the tool.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#ifndef DEVICE_BUILD

#include "render/r3d_pipeline.h"
#include "triangle_sizes.h"

#define GRID  24 /* quads a side */
#define SPACE 8  /* position ticks between grid lines */

typedef struct {
    int16_t (*positions)[3];
    uint8_t (*colors)[3];
    uint16_t (*triangles)[3];
    r3d_lit_cluster_t cluster;
    r3d_lit_node_t node;
    r3d_lit_mesh_t mesh;
} grid_mesh_t;

/* A flat square grid facing +z, centred on the origin, one cluster. */
static void
grid_open(grid_mesh_t* g) {
    const int side = GRID + 1;
    g->positions = malloc(sizeof(*g->positions) * (size_t)(side * side));
    g->colors = calloc((size_t)(side * side), sizeof(*g->colors));
    g->triangles = malloc(sizeof(*g->triangles) * (size_t)(GRID * GRID * 2));
    TEST_ASSERT_NOT_NULL(g->positions);
    TEST_ASSERT_NOT_NULL(g->colors);
    TEST_ASSERT_NOT_NULL(g->triangles);
    const int half = GRID * SPACE / 2;
    for (int j = 0; j < side; j++) {
        for (int i = 0; i < side; i++) {
            int16_t* p = g->positions[(j * side) + i];
            p[0] = (int16_t)((i * SPACE) - half);
            p[1] = (int16_t)((j * SPACE) - half);
            p[2] = 0;
        }
    }
    int t = 0;
    for (int j = 0; j < GRID; j++) {
        for (int i = 0; i < GRID; i++) {
            const uint16_t a = (uint16_t)((j * side) + i);
            const uint16_t b = (uint16_t)(a + 1);
            const uint16_t c = (uint16_t)(a + side + 1);
            const uint16_t d = (uint16_t)(a + side);
            const uint16_t tris[2][3] = {{a, b, c}, {a, c, d}};
            for (int k = 0; k < 2; k++, t++) {
                g->triangles[t][0] = tris[k][0];
                g->triangles[t][1] = tris[k][1];
                g->triangles[t][2] = tris[k][2];
            }
        }
    }
    const int16_t lo[3] = {(int16_t)-half, (int16_t)-half, 0};
    const int16_t hi[3] = {(int16_t)half, (int16_t)half, 0};
    g->cluster = (r3d_lit_cluster_t){
        .vertex_first = 0,
        .vertex_count = (uint16_t)(side * side),
        .triangle_first = 0,
        .triangle_count = (uint16_t)t,
        .lo = {lo[0], lo[1], 0},
        .hi = {hi[0], hi[1], 0},
        .double_sided = false,
    };
    g->node = (r3d_lit_node_t){
        .lo = {lo[0], lo[1], 0},
        .hi = {hi[0], hi[1], 0},
        .first = 0,
        .count = 1,
        .leaf = true,
    };
    g->mesh = (r3d_lit_mesh_t){
        .positions = (const int16_t(*)[3])g->positions,
        .colors = (const uint8_t(*)[3])g->colors,
        .triangles = (const uint16_t(*)[3])g->triangles,
        .clusters = &g->cluster,
        .nodes = &g->node,
        .vertex_count = side * side,
        .triangle_count = t,
        .cluster_count = 1,
        .node_count = 1,
        .position_scale = 1,
    };
}

static void
grid_close(grid_mesh_t* g) {
    free(g->positions);
    free(g->colors);
    free(g->triangles);
}

static r3d_sizes_t
sizes_at(const grid_mesh_t* g, float distance) {
    r3d_lens_t lens;
    const render_view_t frame_view = render_view_fixture_at(
        &(camera_t){{0.0f, 0.0f, distance}, {0.0f, 0.001f, -1.0f}, 0.5f, 1.0f}, (viewport_t){64, 48, 0});
    r3d_lens_init(&lens, &frame_view, g->mesh.position_scale);
    r3d_pipeline_work_t* work = malloc(r3d_pipeline_work_bytes());
    TEST_ASSERT_NOT_NULL(work);
    uint16_t visible[1];
    const int count = r3d_pipeline_cull(&g->mesh, &lens, visible, work);
    r3d_sizes_t s = {0};
    r3d_sizes_count(&g->mesh, &lens, visible, count, &s);
    free(work);
    return s;
}

/* Every drawn triangle lands in exactly one bin, whatever the pose. */
static void
test_the_histogram_adds_up_at_every_pose(void) {
    grid_mesh_t g;
    grid_open(&g);
    static const float distances[] = {60.0f, 400.0f, 2000.0f};
    for (int i = 0; i < 3; i++) {
        const r3d_sizes_t s = sizes_at(&g, distances[i]);
        TEST_ASSERT_GREATER_THAN_INT(0, (int)s.drawn);
        long sum = 0;
        for (int b = 0; b < R3D_SIZES_BINS; b++) {
            sum += s.bins[b];
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)s.drawn, (int)sum, "the bins do not add up to the triangles drawn");
        TEST_ASSERT_TRUE(s.box_empty <= s.bins[R3D_SIZES_ZERO]);
        TEST_ASSERT_TRUE(s.box_two_by_two <= s.drawn);
    }
    grid_close(&g);
}

/* Seen from far away the same grid's triangles cover few centres; seen
 * close, most cover many. */
static void
test_distance_moves_triangles_between_the_bins(void) {
    grid_mesh_t g;
    grid_open(&g);
    const r3d_sizes_t near = sizes_at(&g, 60.0f);
    const r3d_sizes_t far = sizes_at(&g, 2000.0f);
    TEST_ASSERT_GREATER_THAN_INT((int)(near.drawn / 2), (int)near.bins[R3D_SIZES_MORE]);
    TEST_ASSERT_GREATER_THAN_INT((int)(far.drawn / 2), (int)(far.bins[R3D_SIZES_ZERO] + far.bins[R3D_SIZES_ONE]));
    grid_close(&g);
}

static r3d_span_vertex_t
at(float x, float y) {
    return (r3d_span_vertex_t){(int32_t)(x * R3D_SUBPIXEL), (int32_t)(y * R3D_SUBPIXEL), 0.5f, 10, 20, 30};
}

/* A triangle lands in its mode's bin by the centres its box holds, whole,
 * even where the target holds only some of its rows; one with no centre in
 * the target, or no area, is not counted. */
static void
test_a_triangle_counts_by_its_whole_box_and_shading_mode(void) {
    /* Counting reads no pixel, so the targets need none. */
    const r3d_span_target_t whole = r3d_span_target(NULL, NULL, 16, 0, 16);
    const r3d_span_target_t top_rows = r3d_span_target(NULL, NULL, 16, 0, 2);
    r3d_boxes_t* b = calloc(1, sizeof(*b));
    TEST_ASSERT_NOT_NULL(b);
    /* Centres 1..3 across and 1..4 down: a 3 x 4 box. */
    const r3d_span_vertex_t tall[3] = {at(1.2f, 1.2f), at(3.8f, 1.2f), at(1.2f, 4.8f)};
    r3d_boxes_count(b, &top_rows, &tall[0], &tall[1], &tall[2], false);
    r3d_boxes_count(b, &whole, &tall[0], &tall[1], &tall[2], true);
    TEST_ASSERT_EQUAL_INT(1, (int)b->boxes[R3D_BOXES_SMOOTH][3][2]);
    TEST_ASSERT_EQUAL_INT(1, (int)b->boxes[R3D_BOXES_FACE][3][2]);
    /* Two rows and under three pixels wide: flat in either call. */
    const r3d_span_vertex_t tiny[3] = {at(5.2f, 5.2f), at(7.6f, 5.2f), at(5.2f, 6.8f)};
    r3d_boxes_count(b, &whole, &tiny[0], &tiny[1], &tiny[2], true);
    TEST_ASSERT_EQUAL_INT(1, (int)b->boxes[R3D_BOXES_FLAT][1][2]);
    /* Ten centres a side share the last bin. */
    const r3d_span_vertex_t big[3] = {at(0.2f, 0.2f), at(10.2f, 0.2f), at(0.2f, 10.2f)};
    r3d_boxes_count(b, &whole, &big[0], &big[1], &big[2], false);
    TEST_ASSERT_EQUAL_INT(1, (int)b->boxes[R3D_BOXES_SMOOTH][R3D_BOXES_SIDES - 1][R3D_BOXES_SIDES - 1]);
    /* Below the target's rows, and a line with no area. */
    const r3d_span_target_t bottom_rows = r3d_span_target(NULL, NULL, 16, 14, 16);
    r3d_boxes_count(b, &bottom_rows, &tall[0], &tall[1], &tall[2], false);
    const r3d_span_vertex_t line[3] = {at(1.2f, 1.2f), at(3.2f, 3.2f), at(5.2f, 5.2f)};
    r3d_boxes_count(b, &whole, &line[0], &line[1], &line[2], false);
    long all = 0;
    for (int m = 0; m < R3D_BOXES_MODES; m++) {
        for (int r = 0; r < R3D_BOXES_SIDES; r++) {
            for (int c = 0; c < R3D_BOXES_SIDES; c++) {
                all += b->boxes[m][r][c];
            }
        }
    }
    free(b);
    TEST_ASSERT_EQUAL_INT(4, (int)all);
}

/* The table's shares are cumulative: a 3 x 4 box is inside 4 x 4, not 3 x 3. */
static void
test_the_box_table_shares_are_within_each_square(void) {
    r3d_boxes_t* b = calloc(1, sizeof(*b));
    char* text = calloc(1024, 1);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(text);
    b->boxes[R3D_BOXES_SMOOTH][3][2] = 1;
    b->boxes[R3D_BOXES_SMOOTH][0][0] = 1;
    FILE* f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    r3d_boxes_print(f, b);
    rewind(f);
    const size_t n = fread(text, 1, 1023, f);
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
    free(b);
    TEST_ASSERT_GREATER_THAN_INT(0, (int)n);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "| smooth | 2 | 50.0% | 50.0% | 100.0% | 100.0% |"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "| all | 2 | 50.0% | 50.0% | 100.0% | 100.0% |"), text);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, "| flat | 0 | 0.0% |"), text);
    free(text);
}

static char problem[160];

/* NULL when the text reads whole, else the reader's problem. */
static const char*
read_poses_text(const char* text, r3d_sizes_poses_t* out) {
    FILE* f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(fputs(text, f) >= 0);
    rewind(f);
    const bool read = r3d_sizes_read_poses(f, out, problem, sizeof problem);
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
    return read ? NULL : problem;
}

static void
test_a_poses_file_gives_its_size_lens_and_poses(void) {
    r3d_sizes_poses_t* p = malloc(sizeof(*p));
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NULL(read_poses_text("# two views\nsize 64 48\nlens 0.5 1.0\n\npose 0 0 400 0 0.001 -1\n"
                                     "pose 1.5 -2 60 0.25 0 -1\n",
                                     p));
    TEST_ASSERT_EQUAL_INT(64, p->width);
    TEST_ASSERT_EQUAL_INT(48, p->height);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, p->half_fov_short_tan);
    TEST_ASSERT_EQUAL_INT(2, p->count);
    TEST_ASSERT_EQUAL_FLOAT(-2.0f, p->eye[1].y);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, p->forward[1].x);
    free(p);
}

static void
test_a_poses_file_missing_a_part_or_a_number_is_refused(void) {
    r3d_sizes_poses_t* p = malloc(sizeof(*p));
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_NOT_NULL(read_poses_text("size 64 48\npose 0 0 400 0 0 -1\n", p));
    TEST_ASSERT_NOT_NULL(read_poses_text("size 64 48\nlens 0.5 1\n", p));
    TEST_ASSERT_NOT_NULL(read_poses_text("size 64 48\nlens 0.5 1\npose 0 0 400 0 0\n", p));
    TEST_ASSERT_NOT_NULL(read_poses_text("size 64 48\nlens 0.5 1\npose 0 0 400 0 0 -1 7\n", p));
    TEST_ASSERT_NOT_NULL(read_poses_text("size 64 48\nlens 0.5 1\nposes 0 0 400 0 0 -1\n", p));
    TEST_ASSERT_NOT_NULL(strstr(read_poses_text("size 64 48\n# lens next\nlens 0.5\n", p), "line 3"));
    free(p);
}

#define STDIN_POSES "suite_r3d_triangle_sizes_poses.txt"

/* "-" reads the poses from standard input, where a generator pipes them. */
static void
test_poses_read_from_standard_input_when_the_path_is_a_dash(void) {
    r3d_sizes_poses_t* p = malloc(sizeof(*p));
    TEST_ASSERT_NOT_NULL(p);
    FILE* f = fopen(STDIN_POSES, "w");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(fputs("size 32 16\nlens 0.5 1\npose 0 0 50 0 0 -1\n", f) >= 0);
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
    TEST_ASSERT_NOT_NULL(freopen(STDIN_POSES, "r", stdin));
    const bool read = r3d_sizes_read_poses_path("-", p, problem, sizeof problem);
    /* Windows keeps an open file; the host runner never reads stdin. */
    TEST_ASSERT_EQUAL_INT(0, fclose(stdin));
    TEST_ASSERT_EQUAL_INT(0, remove(STDIN_POSES));
    TEST_ASSERT_TRUE_MESSAGE(read, problem);
    TEST_ASSERT_EQUAL_INT(32, p->width);
    TEST_ASSERT_EQUAL_INT(1, p->count);
    TEST_ASSERT_FALSE(r3d_sizes_read_poses_path("no-such-poses-file.txt", p, problem, sizeof problem));
    free(p);
}

void
run_r3d_triangle_sizes_suite(void) {
    RUN_TEST(test_the_histogram_adds_up_at_every_pose);
    RUN_TEST(test_distance_moves_triangles_between_the_bins);
    RUN_TEST(test_a_triangle_counts_by_its_whole_box_and_shading_mode);
    RUN_TEST(test_the_box_table_shares_are_within_each_square);
    RUN_TEST(test_a_poses_file_gives_its_size_lens_and_poses);
    RUN_TEST(test_a_poses_file_missing_a_part_or_a_number_is_refused);
    RUN_TEST(test_poses_read_from_standard_input_when_the_path_is_a_dash);
}

#else

void
run_r3d_triangle_sizes_suite(void) {}

#endif

SUITE_REGISTER(run_r3d_triangle_sizes_suite);

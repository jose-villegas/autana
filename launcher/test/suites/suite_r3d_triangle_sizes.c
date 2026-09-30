/*
 * Host-only suite: tools/r3d/triangle_sizes.h, the triangle-size histogram
 * and its poses file, on a mesh built inside the test. No firmware image
 * compiles the tool.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#ifndef DEVICE_BUILD

#include "render/r3d_lit_pipeline.h"
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
    g->cluster =
        (r3d_lit_cluster_t){0, (uint16_t)(side * side), 0, (uint16_t)t, {lo[0], lo[1], 0}, {hi[0], hi[1], 0}, false};
    g->node = (r3d_lit_node_t){{lo[0], lo[1], 0}, {hi[0], hi[1], 0}, 0, 1, true};
    g->mesh = (r3d_lit_mesh_t){(const int16_t(*)[3])g->positions,
                               (const uint8_t(*)[3])g->colors,
                               (const uint16_t(*)[3])g->triangles,
                               &g->cluster,
                               &g->node,
                               side * side,
                               t,
                               1,
                               1,
                               1};
}

static void
grid_close(grid_mesh_t* g) {
    free(g->positions);
    free(g->colors);
    free(g->triangles);
}

static r3d_sizes_t
sizes_at(const grid_mesh_t* g, float distance) {
    r3d_lit_view_t view;
    r3d_lit_view_look(&view, (r3d_vec3f_t){0.0f, 0.0f, distance}, (r3d_vec3f_t){0.0f, 0.001f, -1.0f}, 0.5f, 1.0f,
                      g->mesh.position_scale, (r3d_viewport_t){64, 48, 0});
    uint16_t visible[1];
    const int count = r3d_lit_cull_clusters(&g->mesh, &view, visible);
    r3d_sizes_t s = {0};
    r3d_sizes_count(&g->mesh, &view, visible, count, &s);
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

static const char*
read_poses_text(const char* text, r3d_sizes_poses_t* out) {
    FILE* f = tmpfile();
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(fputs(text, f) >= 0);
    rewind(f);
    const char* problem = r3d_sizes_read_poses(f, out);
    TEST_ASSERT_EQUAL_INT(0, fclose(f));
    return problem;
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
    free(p);
}

void
run_r3d_triangle_sizes_suite(void) {
    RUN_TEST(test_the_histogram_adds_up_at_every_pose);
    RUN_TEST(test_distance_moves_triangles_between_the_bins);
    RUN_TEST(test_a_poses_file_gives_its_size_lens_and_poses);
    RUN_TEST(test_a_poses_file_missing_a_part_or_a_number_is_refused);
}

#else

void
run_r3d_triangle_sizes_suite(void) {}

#endif

SUITE_REGISTER(run_r3d_triangle_sizes_suite);

#ifndef DEVICE_BUILD
#define r3d_span_small_fill_resolve  constant_resolve
#define r3d_span_small_fill_default  constant_default
#define r3d_span_triangle            constant_r3d_span_triangle
#define r3d_span_triangle_solid      constant_r3d_span_triangle_solid
#define r3d_span_triangle_write      constant_r3d_span_triangle_write
#define r3d_span_hidden              constant_r3d_span_hidden
#define r3d_span_plane_bound         constant_r3d_span_plane_bound
#define r3d_span_plane_in_range      constant_r3d_span_plane_in_range
#define r3d_span_stop_after          constant_r3d_span_stop_after
#define r3d_lens_init                constant_r3d_lens_init
#define r3d_lens_fit                 constant_r3d_lens_fit
#define r3d_lens_place               constant_r3d_lens_place
#define r3d_pipeline_work_bytes      constant_r3d_pipeline_work_bytes
#define r3d_pipeline_cull            constant_r3d_pipeline_cull
#define r3d_pipeline_transform       constant_r3d_pipeline_transform
#define r3d_pipeline_transform_split constant_r3d_pipeline_transform_split
#define r3d_pipeline_draw_split      constant_r3d_pipeline_draw_split
#define r3d_pipeline_draw            constant_r3d_pipeline_draw
#define r3d_pipeline_cluster_hidden  constant_r3d_pipeline_cluster_hidden

#include <stdlib.h>
#include <string.h>
#include "asset/asset_pack.h"
#include "hash/crc32.h"
#include "render/r3d_pipeline.c"
#include "render/r3d_span.c"
#include "suites.h"
#include "unity.h"

#define TEST_SIDE   8
#define TEST_PIXELS (TEST_SIDE * TEST_SIDE)

static uint16_t* pixels;
static r3d_pipeline_work_t* work;

typedef struct {
    r3d_span_small_fill_t resolved;
    r3d_pipeline_vertex_t transformed[6];
    r3d_span_vertex_t projected[4], front[3];
    r3d_span_target_t expected, actual;
} clipped_fixture_t;

static clipped_fixture_t* clipped;

static void
cleanup(void) {
    free(clipped);
    clipped = NULL;
    free(work);
    work = NULL;
    free(pixels);
    pixels = NULL;
}

static void
test_styles_match_pipeline_colours(void) {
    pixels = calloc(TEST_PIXELS * 4, sizeof(*pixels));
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(pixels);
    work = malloc(r3d_pipeline_work_bytes());
    TEST_ASSERT_NOT_NULL(work);
    TEST_ASSERT_EQUAL_UINT(8, sizeof(r3d_pipeline_vertex_t));
    const int16_t positions[3][3] = {{0, 0, 2}, {1, 0, 2}, {0, 1, 2}};
    uint8_t colours[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
    const uint16_t triangle[3] = {0, 1, 2};
    const r3d_lit_cluster_t cluster = {.vertex_count = 3};
    const r3d_lit_mesh_t mesh = {.positions = positions, .colors = (const uint8_t (*)[3])colours};
    const r3d_lens_t lens = {.m = MAT4F_IDENTITY,
                             .project = {1.0F, -1.0F, 0.0F, 0.0F, 1.0F},
                             .near_subpixels = 1.0F / R3D_SUBPIXEL,
                             .snap_cx = R3D_SNAP_BIAS,
                             .snap_cy = R3D_SNAP_BIAS};
    r3d_pipeline_vertex_t transformed[3];
    (void)transform_cluster(&mesh, &lens, &cluster, transformed);
    r3d_span_vertex_t reference[3];
    for (int k = 0; k < 3; k++) {
        transformed[k].sx = k == 1 ? TEST_SIDE * R3D_SUBPIXEL : 0;
        transformed[k].sy = k == 2 ? TEST_SIDE * R3D_SUBPIXEL : 0;
        reference[k] = (r3d_span_vertex_t){transformed[k].sx, transformed[k].sy, transformed[k].iz,
                                           colours[k][0],     colours[k][1],     colours[k][2]};
    }
    r3d_span_target_t expected = r3d_span_target(pixels, pixels + TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
    r3d_span_target_t actual =
        r3d_span_target(pixels + TEST_PIXELS * 2, pixels + TEST_PIXELS * 3, TEST_SIDE, 0, TEST_SIDE);
    const r3d_pipeline_vertex_t* vertices[3] = {transformed, transformed + 1, transformed + 2};
    for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
        r3d_span_small_fill_t resolved;
        r3d_span_small_fill_resolve(&resolved, (small_fill_t){TEST_SIDE, 0, style});
        expected.small_fill = actual.small_fill = &resolved;
        memset(pixels, 0, TEST_PIXELS * 4 * sizeof(*pixels));
        r3d_span_triangle(&expected, reference, reference + 1, reference + 2);
        draw_in_front(&mesh, vertices, triangle, NULL, true, &actual, false, work);
        TEST_ASSERT_EQUAL_MEMORY(pixels, pixels + TEST_PIXELS * 2, TEST_PIXELS * 2 * sizeof(*pixels));
        int coloured = 0;
        for (int i = 0; i < TEST_PIXELS; i++) {
            coloured += pixels[i] != 0;
        }
        TEST_ASSERT_GREATER_THAN(0, coloured);
    }
}

static void
test_clipped_and_mixed_clusters_keep_the_target_style(void) {
    pixels = calloc(TEST_PIXELS * 4, sizeof(*pixels));
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(pixels);
    work = malloc(r3d_pipeline_work_bytes());
    TEST_ASSERT_NOT_NULL(work);
    static const int16_t positions[6][3] = {{1, 1, 0}, {32, 0, 4}, {0, 32, 4}, {0, 0, 4}, {24, 0, 4}, {0, 24, 4}};
    static const uint8_t colors[6][3] = {{255, 0, 0},  {0, 255, 0},    {0, 0, 255},
                                         {32, 80, 96}, {128, 112, 48}, {224, 160, 144}};
    static const uint16_t triangles[2][3] = {{0, 1, 2}, {3, 4, 5}};
    const r3d_lit_cluster_t cluster = {.vertex_count = 6, .triangle_count = 2, .double_sided = true};
    const r3d_lit_mesh_t mesh = {
        .positions = positions, .colors = colors, .triangles = triangles, .clusters = &cluster};
    const r3d_lens_t lens = {.m = MAT4F_IDENTITY,
                             .project = {1.0F, -1.0F, 0.0F, 0.0F, 1.0F},
                             .near_subpixels = 1.0F / R3D_SUBPIXEL,
                             .snap_cx = R3D_SNAP_BIAS,
                             .snap_cy = R3D_SNAP_BIAS};
    clipped = malloc(sizeof(*clipped));
    TEST_ASSERT_NOT_NULL(clipped);
    r3d_pipeline_vertex_t* transformed = clipped->transformed;
    (void)transform_cluster(&mesh, &lens, &cluster, transformed);
    build_guard_planes(&lens, work);
    r3d_span_target_t* expected = &clipped->expected;
    r3d_span_target_t* actual = &clipped->actual;
    *expected = r3d_span_target(pixels, pixels + TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
    *actual = r3d_span_target(pixels + TEST_PIXELS * 2, pixels + TEST_PIXELS * 3, TEST_SIDE, 0, TEST_SIDE);
    const clip_vertex_t polygon[4] = {{8.75F, 0.75F, 1, 191.25F, 63.75F, 0},
                                      {32, 0, 4, 0, 255, 0},
                                      {0, 32, 4, 0, 0, 255},
                                      {0.75F, 8.75F, 1, 191.25F, 0, 63.75F}};
    uint32_t checksums[SMALL_FILL_STYLE_COUNT];
    for (small_fill_style_t style = SMALL_FILL_FLAT; style < SMALL_FILL_STYLE_COUNT; style++) {
        r3d_span_small_fill_t* resolved = &clipped->resolved;
        r3d_span_small_fill_resolve(resolved, (small_fill_t){SMALL_FILL_CEILING_FAST, 0, style});
        expected->small_fill = actual->small_fill = resolved;
        for (int mixed = 0; mixed < 2; mixed++) {
            memset(pixels, 0, TEST_PIXELS * 4 * sizeof(*pixels));
            r3d_span_vertex_t* projected = clipped->projected;
            for (int k = 0; k < 4; k++) {
                projected[k] = project(&lens, &polygon[k]);
            }
            r3d_span_triangle(expected, projected, projected + 1, projected + 2);
            r3d_span_triangle(expected, projected, projected + 2, projected + 3);
            if (mixed) {
                r3d_span_vertex_t* front = clipped->front;
                for (int k = 0; k < 3; k++) {
                    const r3d_pipeline_vertex_t* v = transformed + k + 3;
                    front[k] =
                        (r3d_span_vertex_t){v->sx, v->sy, v->iz, colors[k + 3][0], colors[k + 3][1], colors[k + 3][2]};
                }
                r3d_span_triangle(expected, front, front + 1, front + 2);
            }
            draw_triangles(&mesh, &lens, &cluster, cluster.triangle_first, cluster.triangle_first + (mixed ? 2 : 1),
                           transformed, actual, false, work);
            TEST_ASSERT_EQUAL_MEMORY(pixels, pixels + TEST_PIXELS * 2, TEST_PIXELS * 2 * sizeof(*pixels));
            int covered = 0;
            for (int i = 0; i < TEST_PIXELS; i++) {
                covered += pixels[TEST_PIXELS + i] != R3D_DEPTH_EMPTY;
            }
            TEST_ASSERT_GREATER_THAN(0, covered);
        }
        checksums[style] = crc32_update(0, pixels, TEST_PIXELS * sizeof(*pixels));
        for (int previous = SMALL_FILL_FLAT; previous < (int)style; previous++) {
            TEST_ASSERT_NOT_EQUAL(checksums[previous], checksums[style]);
        }
    }
}

/* One draw on `work`: whether it looked for hidden clusters, and whether it
 * found one. */
static bool
hidden_draw(bool hides) {
    begin_hidden_draw(work);
    const bool looked = work->test_hidden;
    if (looked && hides) {
        work->idle_draws = 0;
    }
    return looked;
}

static void
test_the_hidden_test_pauses_while_nothing_hides_and_resumes_when_something_does(void) {
    work = malloc(r3d_pipeline_work_bytes());
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(work);
    for (int fresh = 0; fresh <= UINT8_MAX; fresh++) {
        work->idle_draws = (uint8_t)fresh;
        TEST_ASSERT_TRUE(hidden_draw(false) || fresh < R3D_HIDDEN_PROBE_DRAW);
        TEST_ASSERT_LESS_OR_EQUAL(R3D_HIDDEN_PROBE_DRAW, work->idle_draws);
    }
    work->idle_draws = 0;
    for (int draw = 0; draw < R3D_HIDDEN_IDLE_DRAWS; draw++) {
        TEST_ASSERT_TRUE(hidden_draw(false));
    }
    for (int cycle = 0; cycle < 2; cycle++) {
        for (int draw = 0; draw < R3D_HIDDEN_PAUSE_DRAWS; draw++) {
            TEST_ASSERT_FALSE(hidden_draw(true));
        }
        TEST_ASSERT_TRUE(hidden_draw(cycle == 1));
    }
    for (int draw = 0; draw < R3D_HIDDEN_IDLE_DRAWS; draw++) {
        TEST_ASSERT_TRUE(hidden_draw(false));
    }
    TEST_ASSERT_FALSE(hidden_draw(false));
}

void
run_r3d_style_pipeline_suite(void) {
    RUN_TEST(test_clipped_and_mixed_clusters_keep_the_target_style);
    RUN_TEST(test_styles_match_pipeline_colours);
    RUN_TEST(test_the_hidden_test_pauses_while_nothing_hides_and_resumes_when_something_does);
}

SUITE_REGISTER(run_r3d_style_pipeline_suite);
#endif

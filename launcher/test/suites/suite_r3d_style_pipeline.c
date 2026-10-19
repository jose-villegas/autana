#ifndef DEVICE_BUILD
#define r3d_span_begin               constant_r3d_span_begin
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

#include <stdlib.h>
#include <string.h>
#include "render/r3d_pipeline.c"
#include "render/r3d_span.c"
#include "suites.h"
#include "unity.h"

#define TEST_SIDE   8
#define TEST_PIXELS (TEST_SIDE * TEST_SIDE)

static uint16_t* pixels;
static r3d_pipeline_work_t* work;

static void
cleanup(void) {
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
                             .near_z = 1,
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
    const r3d_span_target_t expected = r3d_span_target(pixels, pixels + TEST_PIXELS, TEST_SIDE, 0, TEST_SIDE);
    const r3d_span_target_t actual =
        r3d_span_target(pixels + TEST_PIXELS * 2, pixels + TEST_PIXELS * 3, TEST_SIDE, 0, TEST_SIDE);
    const r3d_pipeline_vertex_t* vertices[3] = {transformed, transformed + 1, transformed + 2};
    for (small_fill_style_t style = SMALL_FILL_FLAT; style <= SMALL_FILL_DITHER; style++) {
        r3d_span_begin((small_fill_t){TEST_SIDE, 0, style});
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

void
run_r3d_style_pipeline_suite(void) {
    RUN_TEST(test_styles_match_pipeline_colours);
}

SUITE_REGISTER(run_r3d_style_pipeline_suite);
#endif

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

#include <float.h>
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
                             .depth_near = 1.0F,
                             .snap_cx = R3D_SNAP_BIAS,
                             .snap_cy = R3D_SNAP_BIAS};
    r3d_pipeline_vertex_t transformed[3];
    const transform_setup_t setup = transform_setup(&lens);
    (void)transform_cluster(&mesh, &lens, &setup, &cluster, transformed);
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
                             .depth_near = 1.0F,
                             .snap_cx = R3D_SNAP_BIAS,
                             .snap_cy = R3D_SNAP_BIAS};
    clipped = malloc(sizeof(*clipped));
    TEST_ASSERT_NOT_NULL(clipped);
    r3d_pipeline_vertex_t* transformed = clipped->transformed;
    const transform_setup_t setup = transform_setup(&lens);
    (void)transform_cluster(&mesh, &lens, &setup, &cluster, transformed);
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

#define TRANSFORM_REFERENCE_STEPS 257

static void
test_transform_matches_lens_projection_bits(void) {
    static const float scales[] = {0.03125F, 0.7F, 19.3F};
    const r3d_lit_cluster_t cluster = {.vertex_count = 1};
    const uint16_t index = 0;
    for (size_t scale = 0; scale < sizeof scales / sizeof scales[0]; scale++) {
        r3d_lens_t lens = {.m = MAT4F_IDENTITY,
                           .project.near_z = 1.0F,
                           .snap_cx = R3D_SNAP_BIAS + 0.75F,
                           .snap_cy = R3D_SNAP_BIAS - 0.25F};
        lens.m = mat4f_scale_rows(lens.m, (vec3f_t){scales[scale], -scales[scale], 1.0F});
        lens.m.m[0][1] = scales[scale];
        lens.m.m[1][0] = -scales[scale];
        r3d_lens_set_depth_near(&lens, scales[scale]);
        for (int i = 0; i < TRANSFORM_REFERENCE_STEPS; i++) {
            const int16_t positions[1][3] = {
                {(int16_t)(i - TRANSFORM_REFERENCE_STEPS / 2),
                 (int16_t)(TRANSFORM_REFERENCE_STEPS / 2 - i * i % TRANSFORM_REFERENCE_STEPS), (int16_t)i}};
            const r3d_lit_mesh_t mesh = {.positions = positions, .clusters = &cluster};
            r3d_pipeline_vertex_t out = {.sx = INT16_MIN, .sy = INT16_MIN};
            r3d_pipeline_rows_t rows;
            r3d_pipeline_transform(&mesh, &lens, &index, 1, &out, &rows);
            const vec3f_t l = mat4f_apply(&lens.m, (vec3f_t){positions[0][0], positions[0][1], positions[0][2]});
            if (l.z <= lens.project.near_z) {
                TEST_ASSERT_EQUAL_FLOAT(0.0F, out.iz);
                TEST_ASSERT_TRUE(rows.unbounded);
                continue;
            }
            const float inv = (float)R3D_SUBPIXEL * mathf_recip(l.z);
            const float bx = lens.snap_cx + l.x * inv;
            const float by = lens.snap_cy + l.y * inv;
            const bool fast = bx > FAST_LO && bx < FAST_HI && by > FAST_LO && by < FAST_HI;
            TEST_ASSERT_EQUAL(!fast, rows.unbounded);
            if (fast) {
                const float iz = (lens.depth_near / (float)R3D_SUBPIXEL) * inv;
                TEST_ASSERT_EQUAL_MEMORY(&iz, &out.iz, sizeof iz);
                TEST_ASSERT_EQUAL_INT16(r3d_span_unbias(bx), out.sx);
                TEST_ASSERT_EQUAL_INT16(r3d_span_unbias(by), out.sy);
                const clip_vertex_t v = {.x = l.x, .y = l.y, .z = l.z};
                const r3d_span_vertex_t clipped = project(&lens, &v);
                TEST_ASSERT_EQUAL_INT16(out.sx, clipped.x);
                TEST_ASSERT_EQUAL_INT16(out.sy, clipped.y);
                TEST_ASSERT_EQUAL_MEMORY(&out.iz, &clipped.z, sizeof out.iz);
            } else {
                TEST_ASSERT_EQUAL_FLOAT(-1.0F, out.iz);
            }
        }
    }
}

static void
test_transform_nonfinite_paths(void) {
    static const int16_t positions[1][3] = {{1, 1, 2}};
    static const float nonfinite[] = {INFINITY, -INFINITY, NAN};
    const r3d_lit_cluster_t cluster = {.vertex_count = 1};
    const r3d_lit_mesh_t mesh = {.positions = positions, .clusters = &cluster};
    const uint16_t index = 0;
    for (int axis = 0; axis < MAT3F_AXES; axis++) {
        for (size_t i = 0; i < sizeof nonfinite / sizeof nonfinite[0]; i++) {
            r3d_lens_t lens = {
                .m = MAT4F_IDENTITY, .project.near_z = 1.0F, .snap_cx = R3D_SNAP_BIAS, .snap_cy = R3D_SNAP_BIAS};
            r3d_lens_set_depth_near(&lens, lens.project.near_z);
            lens.m.m[axis][MAT3F_AXES] = nonfinite[i];
            r3d_pipeline_vertex_t out = {.sx = INT16_MIN, .sy = INT16_MIN};
            r3d_pipeline_rows_t rows;
            r3d_pipeline_transform(&mesh, &lens, &index, 1, &out, &rows);
            const bool infinite_depth = axis == 2 && nonfinite[i] == INFINITY;
            const bool behind = axis == 2 && nonfinite[i] == -INFINITY;
            TEST_ASSERT_EQUAL_FLOAT(behind || infinite_depth ? 0.0F : -1.0F, out.iz);
            TEST_ASSERT_EQUAL(!infinite_depth, rows.unbounded);
            /* Finite x/y at +inf depth project to the centre with zero inverse depth. */
            TEST_ASSERT_EQUAL_INT16(infinite_depth ? 0 : INT16_MIN, out.sx);
            TEST_ASSERT_EQUAL_INT16(infinite_depth ? 0 : INT16_MIN, out.sy);
        }
    }
}

#define TEST_FAST_ULP_RADIUS   4096U
#define TEST_FLOAT_BITS_STRIDE UINT32_C(65537)

static void
check_fast_bits(uint32_t bits, const transform_setup_t* setup) {
    const float biased = mathf_from_bits(bits);
    TEST_ASSERT_EQUAL(biased > FAST_LO && biased < FAST_HI, biased_inside_fast(bits, setup));
}

static void
test_transform_fast_range_bits(void) {
    const r3d_lens_t lens = {.m = MAT4F_IDENTITY};
    const transform_setup_t setup = transform_setup(&lens);
    const uint32_t edges[] = {mathf_bits(FAST_LO), mathf_bits(FAST_HI)};
    for (size_t edge = 0; edge < sizeof edges / sizeof edges[0]; edge++) {
        for (uint32_t bits = edges[edge] - TEST_FAST_ULP_RADIUS; bits <= edges[edge] + TEST_FAST_ULP_RADIUS; bits++) {
            check_fast_bits(bits, &setup);
        }
    }
    static const float special[] = {0.0F,    -0.0F,    INFINITY,     -INFINITY,     NAN,     -NAN,
                                    FLT_MAX, -FLT_MAX, FLT_TRUE_MIN, -FLT_TRUE_MIN, -FAST_LO};
    for (size_t i = 0; i < sizeof special / sizeof special[0]; i++) {
        check_fast_bits(mathf_bits(special[i]), &setup);
    }
    for (uint64_t bits = 0; bits <= UINT32_MAX; bits += TEST_FLOAT_BITS_STRIDE) {
        check_fast_bits((uint32_t)bits, &setup);
    }
}

void
run_r3d_style_pipeline_suite(void) {
    RUN_TEST(test_transform_matches_lens_projection_bits);
    RUN_TEST(test_transform_nonfinite_paths);
    RUN_TEST(test_transform_fast_range_bits);
    RUN_TEST(test_clipped_and_mixed_clusters_keep_the_target_style);
    RUN_TEST(test_styles_match_pipeline_colours);
}

SUITE_REGISTER(run_r3d_style_pipeline_suite);
#endif

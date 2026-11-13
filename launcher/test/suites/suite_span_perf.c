/* Fixed triangle rows isolate the span shading kernels: Gouraud, solid and
 * textured. */
#ifdef DEVICE_BUILD
#include <stdlib.h>
#include <string.h>
#include "core/timing.h"
#include "render/r3d_span.h"
#include "render/texture/r3d_texture.h"
#include "suites.h"
#include "testkit/bench.h"
#include "unity.h"

#define SPAN_VERTEX_COUNT      3
#define SPAN_SHADING_MODES     2
#define SPAN_WIDTH             64
#define SPAN_HEIGHT            48
#define SPAN_BATCH             256
#define SPAN_ROUNDS            64
#define SPAN_REPS              (SPAN_BATCH * SPAN_ROUNDS)
#define SPAN_SOLID_COLOR       0xFFFF
/* The textured row's texture: one level of 32 texels a side, its texel
 * indices counting through the palette, a palette channel at the pattern's
 * mean and a grey tint, so the timing does not hang on the image. */
#define SPAN_TEXTURE_SIZE_LOG2 5
#define SPAN_TEXTURE_TINT      128
#define SPAN_TEXTURE_LEVELS    1
#define SPAN_TEXTURE_PALETTE   (R3D_TEXTURE_COLOURS * R3D_TEXTURE_PALETTE_STRIDE)
#define SPAN_TEXTURE_BYTES                                                                                             \
    (R3D_TEXTURE_HEADER_SIZE + SPAN_TEXTURE_PALETTE + r3d_texture_level_bytes(SPAN_TEXTURE_SIZE_LOG2))
/* One texel a pixel along both edges of the triangle. */
static const r3d_span_uv_t triangle_uv[] = {{0, 0}, {SPAN_WIDTH, 0}, {0, SPAN_HEIGHT}};

static const r3d_span_vertex_t triangle[] = {
    {0, 0, 0.25F, 255, 32, 64},
    {SPAN_WIDTH * R3D_SUBPIXEL, 0, 0.5F, 32, 255, 64},
    {0, SPAN_HEIGHT* R3D_SUBPIXEL, 0.75F, 64, 32, 255},
};

static void
build_poses(r3d_span_vertex_t poses[SPAN_BATCH][SPAN_VERTEX_COUNT]) {
    /* Increasing z makes every draw pass depth and write, timing setup plus the write path. */
    for (int i = 0; i < SPAN_BATCH; i++) {
        memcpy(poses[i], triangle, sizeof triangle);
        for (int v = 0; v < SPAN_VERTEX_COUNT; v++) {
            poses[i][v].z = triangle[v].z * (float)(i + 1) / SPAN_BATCH;
        }
    }
}

/* The row's LTEX entry, written into `ltex`, SPAN_TEXTURE_BYTES long. */
static void
write_texture(uint8_t* ltex) {
    memset(ltex, 0, SPAN_TEXTURE_BYTES);
    ltex[R3D_TEXTURE_AT_VERSION] = R3D_TEXTURE_VERSION;
    ltex[R3D_TEXTURE_AT_FORMAT] = R3D_TEXTURE_INDEX4;
    ltex[R3D_TEXTURE_AT_SIZE_LOG2] = SPAN_TEXTURE_SIZE_LOG2;
    ltex[R3D_TEXTURE_AT_LEVELS] = SPAN_TEXTURE_LEVELS;
    memset(ltex + R3D_TEXTURE_AT_TINT, SPAN_TEXTURE_TINT, R3D_TEXTURE_TINT_RGB);
    ltex[R3D_TEXTURE_AT_PALETTE_OFF] = R3D_TEXTURE_HEADER_SIZE;
    ltex[R3D_TEXTURE_AT_TEXELS_OFF] = R3D_TEXTURE_HEADER_SIZE + SPAN_TEXTURE_PALETTE;
    for (int i = 0; i < R3D_TEXTURE_COLOURS; i++) {
        memset(ltex + R3D_TEXTURE_HEADER_SIZE + (i * R3D_TEXTURE_PALETTE_STRIDE), R3D_TEXTURE_ONE,
               R3D_TEXTURE_TINT_RGB);
    }
    uint8_t* texels = ltex + R3D_TEXTURE_HEADER_SIZE + SPAN_TEXTURE_PALETTE;
    for (uint32_t i = 0; i < r3d_texture_level_bytes(SPAN_TEXTURE_SIZE_LOG2); i++) {
        texels[i] = (uint8_t)i;
    }
}

static void
report_textured(const r3d_span_target_t* target, uint16_t* depth,
                r3d_span_vertex_t poses[SPAN_BATCH][SPAN_VERTEX_COUNT]) {
    uint8_t* ltex = malloc(SPAN_TEXTURE_BYTES);
    r3d_texture_t* texture = malloc(sizeof *texture);
    TEST_ASSERT_NOT_NULL(ltex);
    TEST_ASSERT_NOT_NULL(texture);
    write_texture(ltex);
    const asset_view_t view = {ltex, SPAN_TEXTURE_BYTES};
    TEST_ASSERT_EQUAL(ASSET_OK, r3d_texture_from_asset(&view, texture));
    int64_t elapsed = 0;
    for (int round = 0; round < SPAN_ROUNDS; round++) {
        memset(depth, R3D_DEPTH_EMPTY, SPAN_WIDTH * SPAN_HEIGHT * sizeof *depth);
        const int64_t start = timing_now_us();
        for (int i = 0; i < SPAN_BATCH; i++) {
            r3d_span_textured_triangle(target, &poses[i][0], &poses[i][1], &poses[i][2], triangle_uv, texture);
        }
        elapsed += timing_now_us() - start;
    }
    bench_icount_report("span_textured", elapsed, SPAN_REPS, SPAN_ROUNDS);
    free(texture);
    free(ltex);
}

static void
report_shading_mode(const r3d_span_target_t* target, uint16_t* depth,
                    r3d_span_vertex_t poses[SPAN_BATCH][SPAN_VERTEX_COUNT], int solid) {
    int64_t elapsed = 0;
    for (int round = 0; round < SPAN_ROUNDS; round++) {
        memset(depth, R3D_DEPTH_EMPTY, SPAN_WIDTH * SPAN_HEIGHT * sizeof *depth);
        const int64_t start = timing_now_us();
        for (int i = 0; i < SPAN_BATCH; i++) {
            if (solid) {
                r3d_span_triangle_solid(target, &poses[i][0], &poses[i][1], &poses[i][2], SPAN_SOLID_COLOR);
            } else {
                r3d_span_triangle(target, &poses[i][0], &poses[i][1], &poses[i][2]);
            }
        }
        elapsed += timing_now_us() - start;
    }
    bench_icount_report(solid ? "span_solid" : "span_gouraud", elapsed, SPAN_REPS, SPAN_ROUNDS);
}

static void
test_span_instruction_rows(void) {
    gfx_color_t* color = calloc(SPAN_WIDTH * SPAN_HEIGHT, sizeof *color);
    uint16_t* depth = calloc(SPAN_WIDTH * SPAN_HEIGHT, sizeof *depth);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    r3d_span_vertex_t(*poses)[SPAN_VERTEX_COUNT] = malloc(SPAN_BATCH * sizeof *poses);
    TEST_ASSERT_NOT_NULL(poses);
    build_poses(poses);
    const r3d_span_target_t target = r3d_span_target(color, depth, SPAN_WIDTH, 0, SPAN_HEIGHT);
    for (int solid = 0; solid < SPAN_SHADING_MODES; solid++) {
        report_shading_mode(&target, depth, poses, solid);
    }
    report_textured(&target, depth, poses);
    free(poses);
    free(depth);
    free(color);
}

void
run_span_icount_suite(void) {
    RUN_TEST(test_span_instruction_rows);
}

SUITE_REGISTER_ON_REQUEST(run_span_icount_suite);
#endif

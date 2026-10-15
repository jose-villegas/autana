/* Fixed triangle rows isolate the two span shading kernels. */
#ifdef DEVICE_BUILD
#include <stdlib.h>
#include <string.h>
#include "core/timing.h"
#include "perf_stats.h"
#include "render/r3d_span.h"
#include "suites.h"
#include "unity.h"

#define SPAN_VERTEX_COUNT  3
#define SPAN_SHADING_MODES 2
#define SPAN_WIDTH         64
#define SPAN_HEIGHT        48
#define SPAN_BATCH         256
#define SPAN_ROUNDS        64
#define SPAN_REPS          (SPAN_BATCH * SPAN_ROUNDS)
#define SPAN_SOLID_COLOR   0xFFFF

static const r3d_span_vertex_t triangle[] = {
    {0, 0, 0.25F, 255, 32, 64},
    {SPAN_WIDTH * R3D_SUBPIXEL, 0, 0.5F, 32, 255, 64},
    {0, SPAN_HEIGHT* R3D_SUBPIXEL, 0.75F, 64, 32, 255},
};

static void
test_span_instruction_rows(void) {
    gfx_color_t* color = calloc(SPAN_WIDTH * SPAN_HEIGHT, sizeof *color);
    uint16_t* depth = calloc(SPAN_WIDTH * SPAN_HEIGHT, sizeof *depth);
    TEST_ASSERT_NOT_NULL(color);
    TEST_ASSERT_NOT_NULL(depth);
    r3d_span_vertex_t(*poses)[SPAN_VERTEX_COUNT] = malloc(SPAN_BATCH * sizeof *poses);
    TEST_ASSERT_NOT_NULL(poses);
    for (int i = 0; i < SPAN_BATCH; i++) {
        memcpy(poses[i], triangle, sizeof triangle);
        for (int v = 0; v < SPAN_VERTEX_COUNT; v++) {
            poses[i][v].z = triangle[v].z * (float)(i + 1) / SPAN_BATCH;
        }
    }
    const r3d_span_target_t target = r3d_span_target(color, depth, SPAN_WIDTH, 0, SPAN_HEIGHT);
    for (int solid = 0; solid < SPAN_SHADING_MODES; solid++) {
        int64_t elapsed = 0;
        for (int round = 0; round < SPAN_ROUNDS; round++) {
            memset(depth, R3D_DEPTH_EMPTY, SPAN_WIDTH * SPAN_HEIGHT * sizeof *depth);
            const int64_t start = timing_now_us();
            for (int i = 0; i < SPAN_BATCH; i++) {
                if (solid) {
                    r3d_span_triangle_solid(&target, &poses[i][0], &poses[i][1], &poses[i][2], SPAN_SOLID_COLOR);
                } else {
                    r3d_span_triangle(&target, &poses[i][0], &poses[i][1], &poses[i][2]);
                }
            }
            elapsed += timing_now_us() - start;
        }
        perf_icount_print(solid ? "span_solid" : "span_gouraud", elapsed, SPAN_REPS);
        TEST_ASSERT_TRUE(elapsed >= PERF_ICOUNT_MIN_TOTAL_US);
    }
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

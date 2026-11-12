#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "image/pixel/rgb565.h"
#include "render/lmsk_format.h"
#include "render/r3d_span.h"
#include "suites.h"
#include "unity.h"

#define MASK_SIDE     8
#define MASK_PIXELS   (MASK_SIDE * MASK_SIDE)
#define MASK_COLOR    RGB565BE(0xFF0000)
#define MASK_SHADINGS 3
#define MASK_DEPTH    ((uint16_t)(R3D_DEPTH_NEAREST / 2))

static uint16_t* pixels;
static lmsk_atlas_t checker; /* rows inside pixels */

static void
cleanup(void) {
    free(pixels);
    pixels = NULL;
    checker = (lmsk_atlas_t){0};
}

static r3d_span_target_t
fixture(void) {
    pixels = calloc(MASK_PIXELS * 3 + (MASK_SIDE * sizeof(uint32_t) / sizeof(*pixels)), sizeof(*pixels));
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(pixels);
    uint16_t* depth = pixels + MASK_PIXELS;
    uint32_t* mask = (void*)(pixels + MASK_PIXELS * 3);
    for (int y = 0; y < MASK_SIDE; y++) {
        mask[y] = 0;
        for (unsigned x = 0; x < LMSK_MASK_WIDTH; x++) {
            mask[y] |= (uint32_t)((x + y) % 2) << x;
        }
    }
    const r3d_span_target_t target = r3d_span_target(pixels, depth, MASK_SIDE, 0, MASK_SIDE);
    checker = (lmsk_atlas_t){mask, MASK_SIDE};
    return target;
}

/* The quad over the target, its u running along x and v along y, or with
 * `turned` the other way round, as a card upright in a picture on its side. */
static void
draw_quad(const r3d_span_target_t* target, int shading, bool turned) {
    enum { SIDE_UV = MASK_SIDE << LMSK_UV_FRACTION_BITS };

    static const r3d_span_vertex_t quad[] = {
        {0, 0, 0.5F, UINT8_MAX, 0, 0},
        {MASK_SIDE * R3D_SUBPIXEL, 0, 0.5F, UINT8_MAX, 0, 0},
        {MASK_SIDE * R3D_SUBPIXEL, MASK_SIDE * R3D_SUBPIXEL, 0.5F, UINT8_MAX, 0, 0},
        {0, MASK_SIDE * R3D_SUBPIXEL, 0.5F, UINT8_MAX, 0, 0},
    };
    static const float along_x[] = {0, SIDE_UV, SIDE_UV, 0}, along_y[] = {0, 0, SIDE_UV, SIDE_UV};
    const float* quad_u = turned ? along_y : along_x;
    const float* quad_v = turned ? along_x : along_y;
    static const int corners[][R3D_SPAN_TRIANGLE_VERTICES] = {{0, 1, 2}, {0, 2, 3}};
    for (int t = 0; t < (int)(sizeof corners / sizeof corners[0]); t++) {
        const r3d_span_vertex_t *a = &quad[corners[t][0]], *b = &quad[corners[t][1]], *c = &quad[corners[t][2]];
        r3d_span_mask_t mask = {&checker, {0}, {0}};
        for (int k = 0; k < R3D_SPAN_TRIANGLE_VERTICES; k++) {
            mask.u[k] = quad_u[corners[t][k]];
            mask.v[k] = quad_v[corners[t][k]];
        }
        const uint16_t color = MASK_COLOR;
        r3d_span_target_t view = *target;
        view.skip_hidden = shading == 2;
        r3d_span_masked_triangle(&view, &mask, a, b, c, shading == 1 ? &color : NULL);
    }
}

static void
draw_checker(const r3d_span_target_t* target, int shading) {
    draw_quad(target, shading, false);
}

static void
test_masked_quad_checker_texels_and_depth(void) {
    const r3d_span_target_t target = fixture();
    uint16_t* depth = pixels + MASK_PIXELS;
    draw_checker(&target, 0);
    for (int y = 0; y < MASK_SIDE; y++) {
        for (int x = 0; x < MASK_SIDE; x++) {
            const bool kept = (x + y) % 2 != 0;
            TEST_ASSERT_EQUAL_HEX16(kept ? MASK_COLOR : 0, pixels[y * MASK_SIDE + x]);
            TEST_ASSERT_EQUAL_UINT16(kept ? MASK_DEPTH : R3D_DEPTH_EMPTY, depth[y * MASK_SIDE + x]);
        }
    }
}

static void
record_written(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int first, int last, int32_t z,
               int32_t dz) {
    uint16_t* hits = pixels + MASK_PIXELS * 2;
    for (int x = first; x <= last; x++) {
        hits[y * MASK_SIDE + x]++;
    }
}

/* Depth before the draw, cycling nearest (the quad loses), the quad's own
 * depth (a tie loses) and empty (the quad wins). */
static uint16_t
depth_before(int i) {
    const int kind = i % MASK_SHADINGS;
    if (kind == 0) {
        return R3D_DEPTH_NEAREST;
    }
    return kind == 1 ? MASK_DEPTH : R3D_DEPTH_EMPTY;
}

static void
assert_only_winners_written(const uint16_t* hits) {
    for (int y = 0; y < MASK_SIDE; y++) {
        for (int x = 0; x < MASK_SIDE; x++) {
            const int i = y * MASK_SIDE + x;
            const bool won = (x + y) % 2 != 0 && depth_before(i) == R3D_DEPTH_EMPTY;
            TEST_ASSERT_EQUAL_UINT16(won ? 1 : 0, hits[i]);
            TEST_ASSERT_EQUAL_HEX16(won ? MASK_COLOR : 0, pixels[i]);
        }
    }
}

static void
test_masked_writers_see_only_depth_winners(void) {
    r3d_span_target_t target = fixture();
    const r3d_span_writer_t writer = {.span = record_written};
    target.writers = &writer;
    target.writer_count = 1;
    uint16_t* depth = pixels + MASK_PIXELS;
    uint16_t* hits = pixels + MASK_PIXELS * 2;
    for (int shading = 0; shading < MASK_SHADINGS; shading++) {
        memset(pixels, 0, MASK_PIXELS * 3 * sizeof(*pixels));
        for (int i = 0; i < MASK_PIXELS; i++) {
            depth[i] = depth_before(i);
        }
        draw_checker(&target, shading);
        assert_only_winners_written(hits);
    }
}

/* A span reads every atlas row its v crosses: with v along x, row r keeping
 * texels below r keeps pixel (x, y) where y < x, which no one row a span
 * could give. */
static void
test_masked_span_reads_each_row_its_v_crosses(void) {
    const r3d_span_target_t target = fixture();
    uint32_t* rows = (uint32_t*)(uintptr_t)checker.rows;
    for (int r = 0; r < MASK_SIDE; r++) {
        rows[r] = (1U << r) - 1U;
    }
    draw_quad(&target, 0, true);
    for (int y = 0; y < MASK_SIDE; y++) {
        for (int x = 0; x < MASK_SIDE; x++) {
            TEST_ASSERT_EQUAL_HEX16(y < x ? MASK_COLOR : 0, pixels[y * MASK_SIDE + x]);
        }
    }
}

void
run_masked_mesh_suite(void) {
    RUN_TEST(test_masked_span_reads_each_row_its_v_crosses);
    RUN_TEST(test_masked_quad_checker_texels_and_depth);
    RUN_TEST(test_masked_writers_see_only_depth_winners);
}

SUITE_REGISTER(run_masked_mesh_suite);

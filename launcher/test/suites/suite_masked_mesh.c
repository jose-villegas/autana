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

static void
cleanup(void) {
    free(pixels);
    pixels = NULL;
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
    r3d_span_target_t target = r3d_span_target(pixels, depth, MASK_SIDE, 0, MASK_SIDE);
    target.mask_rows = mask;
    target.mask_row_count = MASK_SIDE;
    return target;
}

static void
draw_checker(const r3d_span_target_t* target, int shading) {
    static const r3d_span_vertex_t quad[] = {
        {0, 0, 0.5F, UINT8_MAX, 0, 0, 0, 0},
        {MASK_SIDE * R3D_SUBPIXEL, 0, 0.5F, UINT8_MAX, 0, 0, MASK_SIDE << LMSK_UV_FRACTION_BITS, 0},
        {MASK_SIDE * R3D_SUBPIXEL, MASK_SIDE * R3D_SUBPIXEL, 0.5F, UINT8_MAX, 0, 0, MASK_SIDE << LMSK_UV_FRACTION_BITS,
         MASK_SIDE << LMSK_UV_FRACTION_BITS},
        {0, MASK_SIDE * R3D_SUBPIXEL, 0.5F, UINT8_MAX, 0, 0, 0, MASK_SIDE << LMSK_UV_FRACTION_BITS},
    };
    static const int corners[][3] = {{0, 1, 2}, {0, 2, 3}};
    for (int t = 0; t < (int)(sizeof corners / sizeof corners[0]); t++) {
        const r3d_span_vertex_t *a = &quad[corners[t][0]], *b = &quad[corners[t][1]], *c = &quad[corners[t][2]];
        if (shading == 0) {
            r3d_span_triangle(target, a, b, c);
        } else if (shading == 1) {
            r3d_span_triangle_solid(target, a, b, c, MASK_COLOR);
        } else {
            r3d_span_capped_triangle(target, a, b, c, NULL);
        }
    }
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
            depth[i] = i % MASK_SHADINGS == 0   ? R3D_DEPTH_NEAREST
                       : i % MASK_SHADINGS == 1 ? MASK_DEPTH
                                                : R3D_DEPTH_EMPTY;
        }
        draw_checker(&target, shading);
        for (int y = 0; y < MASK_SIDE; y++) {
            for (int x = 0; x < MASK_SIDE; x++) {
                const int i = y * MASK_SIDE + x;
                const bool won = (x + y) % 2 != 0 && i % MASK_SHADINGS == MASK_SHADINGS - 1;
                TEST_ASSERT_EQUAL_UINT16(won ? 1 : 0, hits[i]);
                TEST_ASSERT_EQUAL_HEX16(won ? MASK_COLOR : 0, pixels[i]);
            }
        }
    }
}

void
run_masked_mesh_suite(void) {
    RUN_TEST(test_masked_quad_checker_texels_and_depth);
    RUN_TEST(test_masked_writers_see_only_depth_winners);
}

SUITE_REGISTER(run_masked_mesh_suite);

/*
 * Device-only suite: draw_blend_row() on the PIE unit against its plain C
 * body, byte for byte. Every 16-bit pixel value is blended at every share,
 * from a start that walks the unaligned head through each offset, and rows
 * around the vector minimum cover the short path and the tail. It uses
 * EE.VADDS.S16, which Espressif's QEMU gets wrong, so the board is its check:
 * `autana suite run_draw_blend_suite --flash`.
 */
#include "suites.h" /* portable: needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "core/memory.h"
#include "draw/draw_blend.h"
#include "simd/simd.h"

/* Every 16-bit value once, with room to start it at any pixel within a vector. */
#define EVERY_PIXEL  (UINT16_MAX + 1)
#define HEAD_ROOM    (SIMD_ALIGN / (int)sizeof(gfx_color_t))
#define ROW_PIXELS   (EVERY_PIXEL + HEAD_ROOM)
#define SHARES       (UINT8_MAX + 1)
#define SHORT_ROWS   (DRAW_BLEND_MIN + (3 * DRAW_BLEND_STEP))
#define SHORT_CHECKS 16
#define SHORT_SPAN   (SHORT_ROWS + HEAD_ROOM)

typedef struct {
    void* blocks[2];
    gfx_color_t* pie;
    gfx_color_t* scalar;
} rows_t;

static rows_t
rows_alloc(void) {
    rows_t rows;
    const size_t bytes = ROW_PIXELS * sizeof(gfx_color_t);
    gfx_color_t** at[] = {&rows.pie, &rows.scalar};
    for (int i = 0; i < 2; i++) {
        const memory_aligned_t got = memory_alloc_aligned(bytes, SIMD_ALIGN, MEMORY_APP);
        TEST_ASSERT_NOT_NULL(got.block);
        rows.blocks[i] = got.block;
        *at[i] = got.at;
    }
    return rows;
}

static void
rows_free(rows_t* rows) {
    for (int i = 0; i < 2; i++) {
        memory_free(rows->blocks[i]);
    }
}

/* The colour for `share`: black, white, then any. */
static gfx_color_t
toward_colour(int share) {
    static const gfx_color_t extremes[] = {0, UINT16_MAX};
    return share % 3 < 2 ? extremes[share % 3] : (gfx_color_t)rand();
}

static void
test_every_pixel_at_every_share_matches_the_scalar_blend(void) {
    rows_t rows = rows_alloc();
    srand(1);
    for (int share = 0; share < SHARES; share++) {
        const int head = share % HEAD_ROOM;
        for (int v = 0; v < EVERY_PIXEL; v++) {
            rows.scalar[head + v] = (gfx_color_t)v;
        }
        memcpy(rows.pie, rows.scalar, ROW_PIXELS * sizeof(gfx_color_t));
        const rgb565_mix_toward_t m = rgb565be_mix_toward(toward_colour(share), (uint8_t)share);
        draw_blend_row_scalar(rows.scalar + head, &m, EVERY_PIXEL);
        draw_blend_row(rows.pie + head, &m, EVERY_PIXEL);
        TEST_ASSERT_EQUAL_HEX16_ARRAY(rows.scalar, rows.pie, ROW_PIXELS);
    }
    rows_free(&rows);
}

static void
test_short_rows_and_tails_match_the_scalar_blend(void) {
    rows_t rows = rows_alloc();
    srand(2);
    for (int check = 0; check < SHORT_CHECKS; check++) {
        for (int count = 0; count <= SHORT_ROWS; count++) {
            const int head = (check + count) % HEAD_ROOM;
            for (int x = 0; x < SHORT_SPAN; x++) {
                rows.scalar[x] = (gfx_color_t)rand();
            }
            memcpy(rows.pie, rows.scalar, SHORT_SPAN * sizeof(gfx_color_t));
            const rgb565_mix_toward_t m = rgb565be_mix_toward((gfx_color_t)rand(), (uint8_t)rand());
            draw_blend_row_scalar(rows.scalar + head, &m, (size_t)count);
            draw_blend_row(rows.pie + head, &m, (size_t)count);
            TEST_ASSERT_EQUAL_HEX16_ARRAY(rows.scalar, rows.pie, SHORT_SPAN);
        }
    }
    rows_free(&rows);
}

void
run_draw_blend_suite(void) {
    RUN_TEST(test_every_pixel_at_every_share_matches_the_scalar_blend);
    RUN_TEST(test_short_rows_and_tails_match_the_scalar_blend);
}

#else

void
run_draw_blend_suite(void) {}

#endif

SUITE_REGISTER_ON_REQUEST(run_draw_blend_suite);

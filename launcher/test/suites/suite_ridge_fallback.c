#include "suites.h"

#ifndef DEVICE_BUILD
#include <stdlib.h>

#include "unity.h"

#include "asset/asset_store.h"
#include "core/memory.h"
#include "draw/draw_dither.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#include "shell/ui/ui_ridge.h"

/* The level fallback leaves broad regions of solid land and shaded sky. */
#define MIN_REGION_PIXELS (GFX_WIDTH / 4)
#define FRAME_PIXELS      ((size_t)GFX_WIDTH * GFX_HEIGHT)

static void
test_a_missing_curve_paints_a_level_nonblank_frame(void) {
    TEST_ASSERT_TRUE(gfx_init());
    ui_ridge_step(&(input_t){0}, 0);
    gfx_color_t* frame = memory_alloc(FRAME_PIXELS * sizeof *frame, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(frame);
    ui_ridge_paint_reference_for_test(frame);
    bool level = true;
    bool nonblank = false;
    const int phase_rows = draw_dither_pattern(DRAW_DITHER_SCANLINES4)->height;
    for (int y = 0; y < GFX_HEIGHT; y++) {
        for (int x = 0; x < GFX_WIDTH; x++) {
            const gfx_color_t pixel = frame[(size_t)y * GFX_WIDTH + x];
            nonblank = nonblank || pixel != 0;
            level = level && pixel == frame[(size_t)(y % phase_rows) * GFX_WIDTH + x];
        }
    }
    int run = 1, longest = 1;
    gfx_color_t land = frame[0];
    for (int x = 1; x < GFX_WIDTH; x++) {
        run = frame[x] == frame[x - 1] ? run + 1 : 1;
        if (run > longest) {
            longest = run;
            land = frame[x];
        }
    }
    int sky = 0;
    for (int x = 0; x < GFX_WIDTH; x++) {
        sky += frame[x] != land;
    }
    const bool sky_and_land = longest >= MIN_REGION_PIXELS && sky >= MIN_REGION_PIXELS;
    memory_free(frame);
    gfx_reset_for_test();
    TEST_ASSERT_EQUAL_INT(0, asset_store_mounted());
    TEST_ASSERT_TRUE(sky_and_land);
    TEST_ASSERT_TRUE(level);
    TEST_ASSERT_TRUE(nonblank);
}
#endif

void
suite_ridge_fallback(void) {
#ifndef DEVICE_BUILD
    /* The fallback log comes from the first load; building and repeated loads must not repeat it. */
    ui_ridge_reset_for_test();
    ui_ridge_load();
    TEST_ASSERT_NULL(ui_ridge_curve());
    ui_ridge_load();
    ui_ridge_set_ambient(false);
    ui_ridge_settle();
    RUN_TEST(test_a_missing_curve_paints_a_level_nonblank_frame);
#endif
}

SUITE_REGISTER_ON_REQUEST(suite_ridge_fallback);
#ifndef DEVICE_BUILD
SUITE_READS(suite_ridge_fallback, ASSET_ENGINE_PACK);
#endif

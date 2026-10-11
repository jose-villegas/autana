#include "suites.h"

#ifndef DEVICE_BUILD
#include <stdlib.h>

#include "unity.h"

#include "asset/asset_store.h"
#include "core/memory.h"
#include "gfx/draw/gfx_dither.h"
#include "gfx/gfx.h"
#include "ui/ui_icons.h"
#include "ui/ui_ridge.h"

#define FRAME_PIXELS ((size_t)GFX_WIDTH * GFX_HEIGHT)

static void
test_a_missing_curve_paints_a_level_nonblank_frame(void) {
    gfx_color_t* frame = memory_alloc(FRAME_PIXELS * sizeof *frame, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(frame);
    ui_ridge_paint_reference_for_test(frame);
    bool level = true;
    bool nonblank = false;
    const int phase_rows = gfx_dither_pattern(GFX_DITHER_SCANLINES4)->height;
    for (int y = 0; y < GFX_HEIGHT; y++) {
        for (int x = 0; x < GFX_WIDTH; x++) {
            const gfx_color_t pixel = frame[(size_t)y * GFX_WIDTH + x];
            nonblank = nonblank || pixel != 0;
            level = level && pixel == frame[(size_t)(y % phase_rows) * GFX_WIDTH + x];
        }
    }
    memory_free(frame);
    TEST_ASSERT_TRUE(level);
    TEST_ASSERT_TRUE(nonblank);
}
#endif

void
suite_ridge_fallback(void) {
#ifndef DEVICE_BUILD
    if (asset_store_mounted() == 0) {
        ui_ridge_load();
        ui_ridge_set_ambient(false);
        ui_ridge_settle();
        RUN_TEST(test_a_missing_curve_paints_a_level_nonblank_frame);
    }
#endif
}

SUITE_REGISTER_ON_REQUEST(suite_ridge_fallback);
#ifndef DEVICE_BUILD
SUITE_READS(suite_ridge_fallback, UI_ENGINE_PACK);
#endif

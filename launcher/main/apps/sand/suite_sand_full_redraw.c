/*
 * Device suite: gfx_request_full_redraw() (gfx.h) reaching a real running
 * app end to end - RUNSUITE-only, since it needs the real panel, the real
 * framebuffer, and app_sand.c, none of which link on a host.
 *
 * app_sand.c is the hardware-facing entry point and stays unlinked on a
 * host build; sand_app_enter_running_for_test() is its own SELFTEST-only
 * hook (like sand_app_alloc_selfcheck() in suite_sand_perf.c) for reaching
 * a running grid without a real touch drag through microui.
 */

#include "suites.h"
#include "unity.h"

#ifdef DEVICE_BUILD

#include <string.h>

#include "app.h"
#include "board/board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gfx/gfx.h"

extern const app_t app_sand;
extern void sand_app_enter_running_for_test(void);

static const char* TAG = "suite_sand_full_redraw";

/* Applies a pending gfx_request_full_redraw() exactly the way main.c's
 * apply_pending_full_redraw() does: consumed before the app draws, so a
 * request made inside the app's own frame() reaches the pass that
 * follows rather than this one. */
static void
apply_pending_full_redraw(void) {
    if (!gfx_full_redraw_pending()) {
        return;
    }
    gfx_full_redraw_clear_pending();
    app_sand.invalidate();
}

/* A rectangle well inside the default-quality grid's own drawable area
 * (368x448 with zero margin at NORMAL/cell=4) and clear of the one grain
 * sand_app_enter_running_for_test() pours at the grid's centre - stale
 * pixels an overlay panel would leave behind, per Launcher-Architecture.md's
 * "black rectangular cut-outs" symptom. */
#define STALE_X 60
#define STALE_Y 60
#define STALE_W 40
#define STALE_H 40

static void
test_full_redraw_repaints_a_stale_rect_and_sends_every_strip(void) {
    input_t idle = {0};

    app_sand.enter();
    sand_app_enter_running_for_test();
    apply_pending_full_redraw();
    app_sand.frame(0, &idle);
    gfx_present();

    const size_t fb_bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    gfx_color_t* reference = heap_caps_malloc(fb_bytes, BOARD_FRAMEBUFFER_CAPS);
    TEST_ASSERT_NOT_NULL_MESSAGE(reference, "could not allocate a framebuffer-sized reference copy");
    memcpy(reference, gfx_framebuffer(), fb_bytes);

    /* Settles on the panel and in gfx's own dirty tracker, exactly like a
     * real overlay's own draw+present would - sand's row-run/dirty-column
     * state never learns this rectangle exists. */
    gfx_fill_rect(STALE_X, STALE_Y, STALE_W, STALE_H, gfx_rgb(0xFF00FF));
    gfx_present();

    gfx_request_full_redraw();
    apply_pending_full_redraw();

    gfx_reset_strip_send_counts();
    app_sand.frame(0, &idle);
    gfx_present();

    int full_bands = 0, gathered = 0, partial_bands = 0;
    gfx_get_strip_send_counts(&full_bands, &gathered, &partial_bands);
    ESP_LOGI(TAG, "strips sent: full=%d gathered=%d partial=%d", full_bands, gathered, partial_bands);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, gathered, "a full redraw must send whole strips, not gathered runs");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, partial_bands, "a full redraw must send whole strips, not a partial one");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, full_bands, "every strip must actually be sent");

    const int mismatch = memcmp(reference, gfx_framebuffer(), fb_bytes) != 0;
    ESP_LOGI(TAG, "framebuffer matches the clean reference: %s", mismatch ? "NO" : "yes");
    free(reference);
    TEST_ASSERT_FALSE_MESSAGE(mismatch, "a full redraw must repaint every stale pixel, the palette-panel-ghost "
                                        "and black-cutout class of bug");

    app_sand.exit();
}

#endif /* DEVICE_BUILD */

void
run_sand_full_redraw_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_full_redraw_repaints_a_stale_rect_and_sends_every_strip);
#endif
}

SUITE_REGISTER(run_sand_full_redraw_suite);

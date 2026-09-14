/*
 * Portable suite: the framebuffer-availability guard (gfx_fb_guard.h) every
 * pixel-writing gfx_* entry point checks before touching the framebuffer -
 * driven directly, the same reason suite_gfx_present_guard.c drives
 * gfx_present_guard.h without gfx.c or a panel.
 *
 * This is the layer gfx.c's real primitives (gfx_clear(), gfx_fill_rect(),
 * gfx_pixel(), the line and text functions, gfx_framebuffer() itself) all
 * delegate to once band mode (gfx_mode.h) has freed the PSRAM framebuffer -
 * proving IT never lets a caller through is what proves none of them can
 * write into a buffer that no longer exists, without needing gfx.c's own
 * device-only allocation and panel plumbing.
 */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_fb_guard.h"

static void
fixture(void) {
    gfx_fb_guard_available = true;
    gfx_fb_guard_trips = 0;
}

static void
test_drawing_is_allowed_while_a_framebuffer_is_available(void) {
    fixture();
    TEST_ASSERT_TRUE(gfx_fb_guard_ok());
    TEST_ASSERT_EQUAL_UINT(0, gfx_fb_guard_trips);
}

/* The exact scenario the bug report named: main.c's home hint (or any
 * other shell-side draw) reaching a gfx_* entry point after band mode has
 * freed the framebuffer must be refused, not crash. */
static void
test_drawing_is_refused_once_the_framebuffer_is_unavailable(void) {
    fixture();
    gfx_fb_guard_set_available(false);

    TEST_ASSERT_FALSE(gfx_fb_guard_ok());
    TEST_ASSERT_EQUAL_UINT(1, gfx_fb_guard_trips);
}

static void
test_repeated_draws_while_unavailable_keep_tripping(void) {
    fixture();
    gfx_fb_guard_set_available(false);

    TEST_ASSERT_FALSE(gfx_fb_guard_ok());
    TEST_ASSERT_FALSE(gfx_fb_guard_ok());
    TEST_ASSERT_FALSE(gfx_fb_guard_ok());
    TEST_ASSERT_EQUAL_UINT(3, gfx_fb_guard_trips);
}

/* gfx_mode_exit() (gfx.c) calls gfx_fb_guard_set_available(true) once the
 * framebuffer is reallocated - drawing must resume cleanly, with no trip
 * left over from while it was gone. */
static void
test_restoring_the_framebuffer_stops_the_guard_from_tripping(void) {
    fixture();
    gfx_fb_guard_set_available(false);
    TEST_ASSERT_FALSE(gfx_fb_guard_ok());

    gfx_fb_guard_set_available(true);
    TEST_ASSERT_TRUE(gfx_fb_guard_ok());
    TEST_ASSERT_EQUAL_UINT(1, gfx_fb_guard_trips);
}

void
run_gfx_fb_guard_suite(void) {
    RUN_TEST(test_drawing_is_allowed_while_a_framebuffer_is_available);
    RUN_TEST(test_drawing_is_refused_once_the_framebuffer_is_unavailable);
    RUN_TEST(test_repeated_draws_while_unavailable_keep_tripping);
    RUN_TEST(test_restoring_the_framebuffer_stops_the_guard_from_tripping);
}

SUITE_REGISTER(run_gfx_fb_guard_suite);

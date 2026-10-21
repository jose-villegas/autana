/* Device-only: at the fast panel clock every send is full-width rows, since
 * there the panel sometimes lands a partial-width window a column off; at
 * the slow clock a small change still goes as a partial window. */
#include "suites.h"

#ifdef DEVICE_BUILD

#include "unity.h"

#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_present.h"

#define WINDOWS_MAX 32

/* A box well inside the panel, narrower than a grid column. */
#define BOX_X       200
#define BOX_Y       100
#define BOX_W       20
#define BOX_H       10

/* Sends a small change at `hz` and returns how many of its windows were
 * narrower than the panel. */
static int
partial_windows_at(int hz, int* windows) {
    gfx_send_window_t log[WINDOWS_MAX];
    gfx_set_panel_clock_hz(hz);
    gfx_mark_all_dirty();
    gfx_present();
    gfx_set_send_window_log(log, WINDOWS_MAX);
    gfx_mark_dirty(BOX_X, BOX_Y, BOX_W, BOX_H);
    gfx_present();
    *windows = gfx_send_window_count();
    gfx_set_send_window_log(NULL, 0);
    int partial = 0;
    for (int i = 0; i < *windows && i < WINDOWS_MAX; i++) {
        partial += log[i].x0 != 0 || log[i].x1 != GFX_WIDTH;
    }
    return partial;
}

void
test_present_fast_clock_sends_full_width_rows(void) {
    const int restore_hz = gfx_panel_clock_hz();
    int fast_windows = 0;
    int slow_windows = 0;
    const int fast_partial = partial_windows_at(GFX_PANEL_CLOCK_FAST_HZ, &fast_windows);
    const int slow_partial = partial_windows_at(GFX_PANEL_CLOCK_SLOW_HZ, &slow_windows);
    gfx_set_panel_clock_hz(restore_hz);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, fast_windows, "the change was never sent at the fast clock");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, slow_windows, "the change was never sent at the slow clock");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fast_partial, "a partial-width window went out at the fast clock");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, slow_partial, "the slow clock no longer sends a small change narrow");
}

void
run_present_columns_suite(void) {
    RUN_TEST(test_present_fast_clock_sends_full_width_rows);
}

#else

void
run_present_columns_suite(void) {}

#endif

SUITE_REGISTER(run_present_columns_suite);

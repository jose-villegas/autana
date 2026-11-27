/* Device-only: what one present sends, read from the send-window log. At the
 * fast panel clock every send is full-width rows over just the rows that
 * changed, since there the panel sometimes lands a partial-width window a
 * column off; at the slow clock a small change still goes as a partial
 * window. With the cell overlay on, every strip still goes out once per
 * present: a strip bordered by the last present and dirty again is sent once
 * with the new borders, not resent clean first; a quiet strip is resent clean
 * once, to wipe its old borders. Only last present's borders come off: a box
 * resent to take them off is not bordered again. */
#include "suites.h"

#ifdef DEVICE_BUILD

#include <stdlib.h>

#include "unity.h"

#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/send/gfx_dirty.h"
#include "math/scalar/mathi.h"

/* The most sends one present makes: per strip, each of its GRID_COLS runs
 * split up to LEAF_REFINE_MAX_RUNS ways. The log holds one more, so a full log
 * shows a present that sent more than that, past what was counted. */
#define SENDS_PER_PRESENT_MAX (STRIP_COUNT * GRID_COLS * LEAF_REFINE_MAX_RUNS)
#define WINDOWS_MAX           (SENDS_PER_PRESENT_MAX + 1)

/* A box well inside the panel's second strip, narrower than a grid column. */
#define BOX_X                 200
#define BOX_Y                 (STRIP_HEIGHT + 10)
#define BOX_W                 20
#define BOX_H                 10
/* A second box in the same strip and column, on rows below the first. */
#define BOX_B_Y               (BOX_Y + 30)

/* Presents with the send-window log on; returns how many windows went out. */
static int
present_logged(gfx_send_window_t* log) {
    gfx_set_send_window_log(log, WINDOWS_MAX);
    gfx_present();
    const int windows = gfx_send_window_count();
    gfx_set_send_window_log(NULL, 0);
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(WINDOWS_MAX, windows, "the send-window log filled up: sends went uncounted");
    return windows;
}

/* Sends one changed box at `hz` into `log`; returns how many
 * windows went out. */
static int
send_box_at(int hz, int x, int y, int w, int h, gfx_send_window_t* log) {
    gfx_set_panel_clock_hz(hz);
    gfx_mark_all_dirty();
    gfx_present();
    gfx_mark_dirty(x, y, w, h);
    return present_logged(log);
}

static int
partial_windows(const gfx_send_window_t* log, int windows) {
    int partial = 0;
    for (int i = 0; i < windows; i++) {
        partial += log[i].x0 != 0 || log[i].x1 != GFX_WIDTH;
    }
    return partial;
}

void
test_present_fast_clock_sends_full_width_rows(void) {
    const int restore_hz = gfx_panel_clock_hz();
    gfx_send_window_t* const fast = malloc((WINDOWS_MAX) * sizeof *fast);
    TEST_ASSERT_NOT_NULL(fast);
    gfx_send_window_t* const slow = malloc((WINDOWS_MAX) * sizeof *slow);
    TEST_ASSERT_NOT_NULL(slow);
    const int fast_windows = send_box_at(GFX_PANEL_CLOCK_FAST_HZ, BOX_X, BOX_Y, BOX_W, BOX_H, fast);
    const int slow_windows = send_box_at(GFX_PANEL_CLOCK_SLOW_HZ, BOX_X, BOX_Y, BOX_W, BOX_H, slow);
    gfx_set_panel_clock_hz(restore_hz);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, fast_windows, "the change was never sent at the fast clock");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, slow_windows, "the change was never sent at the slow clock");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, partial_windows(fast, fast_windows),
                                  "a partial-width window went out at the fast clock");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, partial_windows(slow, slow_windows),
                                         "the slow clock no longer sends a small change narrow");
    free(fast);
    free(slow);
}

/* The full-width sends cover exactly the changed rows, rounded out to even
 * edges, one per strip the box touches: never the whole strip for a change
 * shorter than it. */
void
test_present_fast_clock_sends_only_changed_rows(void) {
    const int restore_hz = gfx_panel_clock_hz();
    gfx_send_window_t* const odd = malloc((WINDOWS_MAX) * sizeof *odd);
    TEST_ASSERT_NOT_NULL(odd);
    gfx_send_window_t* const across = malloc((WINDOWS_MAX) * sizeof *across);
    TEST_ASSERT_NOT_NULL(across);
    /* Odd top and bottom edges, inside one strip. */
    const int odd_y = BOX_Y + 1;
    const int odd_windows = send_box_at(GFX_PANEL_CLOCK_FAST_HZ, BOX_X, odd_y, BOX_W, BOX_H, odd);
    /* Straddling the edge between the first two strips, odd edges again. */
    const int across_y = STRIP_HEIGHT - (BOX_H / 2) - 1;
    const int across_windows = send_box_at(GFX_PANEL_CLOCK_FAST_HZ, BOX_X, across_y, BOX_W, BOX_H, across);
    gfx_set_panel_clock_hz(restore_hz);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, odd_windows, "a change inside one strip took more than one send");
    TEST_ASSERT_EQUAL_INT_MESSAGE(mathi_even_floor(odd_y), odd[0].y0, "the send starts off the changed rows");
    TEST_ASSERT_EQUAL_INT_MESSAGE(mathi_even_ceil(odd_y + BOX_H), odd[0].y1, "the send ends off the changed rows");

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, across_windows, "a change across two strips is not one send per strip");
    TEST_ASSERT_EQUAL_INT_MESSAGE(mathi_even_floor(across_y), across[0].y0, "the first strip's send starts off");
    TEST_ASSERT_EQUAL_INT_MESSAGE(STRIP_HEIGHT, across[0].y1, "the first strip's send ends off its edge");
    TEST_ASSERT_EQUAL_INT_MESSAGE(STRIP_HEIGHT, across[1].y0, "the second strip's send starts off its edge");
    TEST_ASSERT_EQUAL_INT_MESSAGE(mathi_even_ceil(across_y + BOX_H), across[1].y1, "the second strip's send ends off");
    free(odd);
    free(across);
}

/* How many sends of the logged present covered each strip's rows. */
static void
sends_per_strip(const gfx_send_window_t* log, int windows, int* per_strip) {
    for (int row = 0; row < STRIP_COUNT; row++) {
        per_strip[row] = 0;
    }
    for (int i = 0; i < windows; i++) {
        for (int row = log[i].y0 / STRIP_HEIGHT; row <= (log[i].y1 - 1) / STRIP_HEIGHT; row++) {
            per_strip[row]++;
        }
    }
}

/* Borders every strip with the overlay on, marks `dirty` (the whole panel
 * when its width is 0), presents once more and counts the sends per strip.
 * Pins the framebuffer path the overlay's normal send takes: no interlace,
 * no expanded or indexed frame. */
static void
overlay_sends_per_strip(int x, int y, int w, int h, int* per_strip) {
    TEST_ASSERT_FALSE_MESSAGE(gfx_frame_expanded(), "the test needs the framebuffer present path");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GFX_LAYOUT_FULL_FB, gfx_mode_current()->layout,
                                  "the test needs the framebuffer present path");
    const bool overlay = gfx_debug_overlay();
    const bool interlace = gfx_interlace_enabled();
    gfx_set_interlace(false);
    gfx_set_debug_overlay(true);
    gfx_mark_all_dirty();
    gfx_present(); /* every strip now carries borders */

    gfx_send_window_t* const log = malloc((WINDOWS_MAX) * sizeof *log);
    TEST_ASSERT_NOT_NULL(log);
    if (w == 0) {
        gfx_mark_all_dirty();
    } else {
        gfx_mark_dirty(x, y, w, h);
    }
    const int windows = present_logged(log);
    sends_per_strip(log, windows, per_strip);
    gfx_set_debug_overlay(overlay);
    gfx_set_interlace(interlace);
    gfx_mark_all_dirty();
    gfx_present();
    free(log);
}

void
test_present_overlay_sends_each_dirty_strip_once(void) {
    int per_strip[STRIP_COUNT];
    overlay_sends_per_strip(0, 0, 0, 0, per_strip);
    for (int row = 0; row < STRIP_COUNT; row++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, per_strip[row], "a bordered strip dirty again went out more than once");
    }
}

void
test_present_overlay_resends_quiet_strips_clean_once(void) {
    int per_strip[STRIP_COUNT];
    overlay_sends_per_strip(BOX_X, BOX_Y, BOX_W, BOX_H, per_strip);
    for (int row = 0; row < STRIP_COUNT; row++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, per_strip[row], "a bordered strip was not sent exactly once");
    }
}

/* Marks the box at rows `y` in strip 1 and presents; returns how many windows
 * went out, the first in `*sent`. */
static int
send_box_rows(int y, gfx_send_window_t* sent) {
    gfx_send_window_t* const log = malloc((WINDOWS_MAX) * sizeof *log);
    TEST_ASSERT_NOT_NULL(log);
    gfx_mark_dirty(BOX_X, y, BOX_W, BOX_H);
    const int windows = present_logged(log);
    *sent = log[0];
    free(log);
    return windows;
}

/* At the slow clock a small change goes as a box of its own. Box A carries
 * borders; marking box B, lower in the same strip, sends one box over both,
 * bordering B; marking B again sends B alone, since A's borders are gone. */
void
test_present_overlay_takes_off_only_the_last_borders(void) {
    TEST_ASSERT_FALSE_MESSAGE(gfx_frame_expanded(), "the test needs the framebuffer present path");
    TEST_ASSERT_EQUAL_INT_MESSAGE(GFX_LAYOUT_FULL_FB, gfx_mode_current()->layout,
                                  "the test needs the framebuffer present path");
    const int restore_hz = gfx_panel_clock_hz();
    const bool overlay = gfx_debug_overlay();
    const bool interlace = gfx_interlace_enabled();
    gfx_set_panel_clock_hz(GFX_PANEL_CLOCK_SLOW_HZ);
    gfx_set_interlace(false);
    gfx_set_debug_overlay(true);
    gfx_mark_all_dirty();
    gfx_present();
    gfx_present(); /* takes every border off: nothing is bordered */

    gfx_send_window_t a;
    gfx_send_window_t both;
    gfx_send_window_t b;
    const int a_windows = send_box_rows(BOX_Y, &a);
    const int both_windows = send_box_rows(BOX_B_Y, &both);
    const int b_windows = send_box_rows(BOX_B_Y, &b);
    gfx_set_debug_overlay(overlay);
    gfx_set_interlace(interlace);
    gfx_set_panel_clock_hz(restore_hz);
    gfx_mark_all_dirty();
    gfx_present();

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, a_windows, "box A did not go out as one send");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, both_windows, "box B and A's take-off did not go out as one send");
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(BOX_Y, both.y0, "the send misses box A's borders");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(BOX_B_Y + BOX_H, both.y1, "the send misses box B");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(STRIP_HEIGHT, both.y1 - both.y0, "the send took the whole strip");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, b_windows, "box B did not go out as one send");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(BOX_Y + BOX_H, b.y0,
                                             "box A went out again: its take-off was bordered and recorded");
}

void
run_present_sends_suite(void) {
    RUN_TEST(test_present_fast_clock_sends_full_width_rows);
    RUN_TEST(test_present_fast_clock_sends_only_changed_rows);
    RUN_TEST(test_present_overlay_sends_each_dirty_strip_once);
    RUN_TEST(test_present_overlay_resends_quiet_strips_clean_once);
    RUN_TEST(test_present_overlay_takes_off_only_the_last_borders);
}

#else

void
run_present_sends_suite(void) {}

#endif

SUITE_REGISTER(run_present_sends_suite);

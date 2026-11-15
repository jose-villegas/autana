/* Device-only: with the cell overlay on, every strip still goes out once per
 * present. A strip bordered by the last present and dirty again is sent once
 * with the new borders, not resent clean first; a strip gone quiet is resent
 * clean once, to wipe its old borders. */
#include "suites.h"

#ifdef DEVICE_BUILD

#include "unity.h"

#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/send/gfx_dirty.h"

#define WINDOWS_MAX (4 * STRIP_COUNT)

/* A box inside the second strip, narrower than a grid column. */
#define BOX_X       200
#define BOX_Y       (STRIP_HEIGHT + 10)
#define BOX_W       20
#define BOX_H       10

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

/* Presents with the log on; returns how many windows went out. */
static int
present_logged(gfx_send_window_t* log) {
    gfx_set_send_window_log(log, WINDOWS_MAX);
    gfx_present();
    const int windows = gfx_send_window_count();
    gfx_set_send_window_log(NULL, 0);
    return windows;
}

void
test_present_overlay_sends_each_dirty_strip_once(void) {
    const bool restore = gfx_debug_overlay();
    gfx_set_debug_overlay(true);
    gfx_mark_all_dirty();
    gfx_present(); /* every strip now carries borders */

    gfx_send_window_t log[WINDOWS_MAX];
    gfx_mark_all_dirty();
    const int windows = present_logged(log);
    int per_strip[STRIP_COUNT];
    sends_per_strip(log, windows, per_strip);
    gfx_set_debug_overlay(restore);
    gfx_mark_all_dirty();
    gfx_present();

    for (int row = 0; row < STRIP_COUNT; row++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, per_strip[row], "a bordered strip dirty again went out more than once");
    }
}

void
test_present_overlay_resends_quiet_strips_clean_once(void) {
    const bool restore = gfx_debug_overlay();
    gfx_set_debug_overlay(true);
    gfx_mark_all_dirty();
    gfx_present(); /* every strip now carries borders */

    gfx_send_window_t log[WINDOWS_MAX];
    gfx_mark_dirty(BOX_X, BOX_Y, BOX_W, BOX_H);
    const int windows = present_logged(log);
    int per_strip[STRIP_COUNT];
    sends_per_strip(log, windows, per_strip);
    gfx_set_debug_overlay(restore);
    gfx_mark_all_dirty();
    gfx_present();

    for (int row = 0; row < STRIP_COUNT; row++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, per_strip[row], "a bordered strip was not sent exactly once");
    }
}

void
run_present_overlay_suite(void) {
    RUN_TEST(test_present_overlay_sends_each_dirty_strip_once);
    RUN_TEST(test_present_overlay_resends_quiet_strips_clean_once);
}

#else

void
run_present_overlay_suite(void) {}

#endif

SUITE_REGISTER(run_present_overlay_suite);

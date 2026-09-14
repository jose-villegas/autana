/*
 * Portable suite: the present-in-flight guard, and its effect on the dirty
 * tracker's own sequencing.
 *
 * gfx_present_guard.h carries no ESP-IDF dependency, the same reason
 * gfx_dirty.h does not - see suite_gfx_dirty.c. Including both here gets an
 * independent copy of each, exactly as gfx.c's own translation unit does,
 * so this exercises the same state gfx_present_begin()/_wait() drive on a
 * real build without needing the panel plumbing gfx.c also carries.
 */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_dirty.h"
#include "gfx/gfx_present_guard.h"

/* This suite only drives the whole-frame path (dirty_mark_all(),
 * dirty_row_is_dirty(), dirty_row_sent(), dirty_frame_sent(), dirty_mark()) -
 * suite_gfx_dirty.c already covers the run/leaf-refinement machinery
 * gfx_dirty.h also carries. Referencing the rest here just keeps this
 * translation unit's own copy of it from tripping -Wunused-function. */
static void
touch_unused_dirty_symbols(void) {
    (void)collect_runs_from_mask;
    (void)collect_dirty_runs;
    (void)run_is_leaf_eligible;
    (void)leaf_mask_for_run;
    (void)refine_run;
    (void)plan_run;
    (void)run_box;
}

static void
fixture(void) {
    touch_unused_dirty_symbols();
    gfx_present_guard_in_flight = false;
    gfx_present_guard_trips = 0;

    cell_dirty = 0;
    all_dirty = false;
    for (int i = 0; i < CELL_COUNT; i++) {
        const int col = i % GRID_COLS;
        const int row = i / GRID_COLS;
        cell_x0[i] = (col + 1) * COL_WIDTH;
        cell_x1[i] = col * COL_WIDTH;
        cell_y0[i] = row * STRIP_HEIGHT + STRIP_HEIGHT;
        cell_y1[i] = row * STRIP_HEIGHT;
    }
    for (int i = 0; i < STRIP_COUNT * LEAF_SUB; i++) {
        leaf_dirty[i] = 0;
    }
}

/* --- the guard itself ------------------------------------------------- */

static void
test_guard_is_quiet_with_no_present_in_flight(void) {
    fixture();
    gfx_present_guard_check();
    TEST_ASSERT_EQUAL_UINT(0, gfx_present_guard_trips);
}

/* Stands in for "a draw call between begin and wait": every gfx_* entry
 * point gfx.c guards calls exactly this function first - see GFX_PRESENT_
 * GUARD() in gfx_present_guard.h. */
static void
test_a_check_between_begin_and_end_trips_the_guard(void) {
    fixture();
    gfx_present_guard_begin();
    gfx_present_guard_check();
    TEST_ASSERT_EQUAL_UINT(1, gfx_present_guard_trips);
}

static void
test_repeated_checks_while_in_flight_keep_tripping(void) {
    fixture();
    gfx_present_guard_begin();
    gfx_present_guard_check();
    gfx_present_guard_check();
    TEST_ASSERT_EQUAL_UINT(2, gfx_present_guard_trips);
}

static void
test_end_stops_the_guard_from_tripping(void) {
    fixture();
    gfx_present_guard_begin();
    gfx_present_guard_check();
    gfx_present_guard_end();
    gfx_present_guard_check();
    TEST_ASSERT_EQUAL_UINT(1, gfx_present_guard_trips);
}

/* --- begin/wait/present sequencing vs. the dirty tracker ---------------- */

/* gfx_present_wait()'s host implementation (gfx.c) is exactly this drain,
 * minus interlace - see dirty_frame_sent()'s own comment for why the whole-
 * frame clear happens once, after every row's own reset. */
static void
drain_like_a_present(void) {
    for (int row = 0; row < STRIP_COUNT; row++) {
        if (dirty_row_is_dirty(row)) {
            dirty_row_sent(row);
        }
    }
    dirty_frame_sent();
}

static void
test_present_sequencing_leaves_every_row_clean(void) {
    fixture();
    dirty_mark_all();

    gfx_present_guard_begin();
    drain_like_a_present();
    gfx_present_guard_end();

    for (int row = 0; row < STRIP_COUNT; row++) {
        TEST_ASSERT_FALSE(dirty_row_is_dirty(row));
    }
    TEST_ASSERT_EQUAL_UINT(0, gfx_present_guard_trips);
}

/* dirty_mark_all() latches all_dirty so a later dirty_mark() call is a
 * no-op for the rest of that frame (gfx_dirty.h) - a present that failed to
 * clear it would silently swallow every mark drawn after the NEXT frame
 * starts, which is exactly the bug a stale flag here would cause. */
static void
test_a_mark_after_present_is_not_swallowed_by_a_stale_all_dirty_flag(void) {
    fixture();
    dirty_mark_all();

    gfx_present_guard_begin();
    drain_like_a_present();
    gfx_present_guard_end();

    dirty_mark(0, 0, 1, 1);
    TEST_ASSERT_TRUE(dirty_row_is_dirty(0));
    TEST_ASSERT_FALSE(dirty_row_is_dirty(1));
}

void
run_gfx_present_guard_suite(void) {
    RUN_TEST(test_guard_is_quiet_with_no_present_in_flight);
    RUN_TEST(test_a_check_between_begin_and_end_trips_the_guard);
    RUN_TEST(test_repeated_checks_while_in_flight_keep_tripping);
    RUN_TEST(test_end_stops_the_guard_from_tripping);

    RUN_TEST(test_present_sequencing_leaves_every_row_clean);
    RUN_TEST(test_a_mark_after_present_is_not_swallowed_by_a_stale_all_dirty_flag);
}

SUITE_REGISTER(run_gfx_present_guard_suite);

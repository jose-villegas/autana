/*
 * Portable suite: the two latches behind gfx_request_full_redraw() (gfx.h).
 *
 * gfx_full_redraw.h carries no ESP-IDF dependency, the same reason
 * gfx_dirty.h and gfx_present_guard.h do not - see suite_gfx_dirty.c.
 * gfx.c's real gfx_request_full_redraw() composes gfx_mark_all_dirty()
 * (dirty_mark_all() here) and gfx_invalidate() (gfx_band_force_all() here)
 * with gfx_full_redraw_latch(); this suite drives that same header state
 * directly, exactly as gfx.c's own translation unit does, with no panel or
 * BSP dependency to satisfy.
 */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_dirty.h"
#include "gfx/gfx_full_redraw.h"

/* This suite only drives dirty_mark_all()/all_dirty - suite_gfx_dirty.c
 * already covers the run/leaf-refinement machinery gfx_dirty.h also
 * carries. Referencing the rest here just keeps this translation unit's own
 * copy of it from tripping -Wunused-function, the same reason
 * suite_gfx_present_guard.c does this. */
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
    all_dirty = false;
    cell_dirty = 0;
    gfx_band_force_all_dirty = false;
    gfx_full_redraw_latched = false;
}

/* band force */

static void
test_band_force_starts_clear_after_the_fixture_resets_it(void) {
    fixture();
    TEST_ASSERT_FALSE(gfx_band_take_force_all());
}

static void
test_band_force_set_and_taken_once(void) {
    fixture();
    gfx_band_force_all();

    TEST_ASSERT_TRUE(gfx_band_take_force_all());
    TEST_ASSERT_FALSE_MESSAGE(gfx_band_take_force_all(), "a second read must not see the same request again");
}

/* the redraw request, composed the way gfx_request_full_redraw() does */

static void
request_full_redraw(void) {
    dirty_mark_all();
    gfx_band_force_all();
    gfx_full_redraw_latch();
}

static void
test_the_request_marks_everything_dirty(void) {
    fixture();
    request_full_redraw();
    TEST_ASSERT_TRUE(all_dirty);
}

static void
test_the_request_forces_the_next_band_frame(void) {
    fixture();
    request_full_redraw();
    TEST_ASSERT_TRUE(gfx_band_take_force_all());
}

static void
test_the_request_latches_pending_for_exactly_one_pass(void) {
    fixture();
    TEST_ASSERT_FALSE_MESSAGE(gfx_full_redraw_is_pending(), "nothing requested yet");

    request_full_redraw();
    TEST_ASSERT_TRUE(gfx_full_redraw_is_pending());

    /* Stands in for main.c's apply_pending_full_redraw(): the shell reads
     * the flag once and clears it before the app's own frame() call. */
    gfx_full_redraw_unlatch();
    TEST_ASSERT_FALSE_MESSAGE(gfx_full_redraw_is_pending(), "consumed for the pass that follows the request");
}

static void
test_a_request_made_after_consuming_the_last_one_latches_again(void) {
    fixture();
    request_full_redraw();
    gfx_full_redraw_unlatch();

    request_full_redraw();
    TEST_ASSERT_TRUE_MESSAGE(gfx_full_redraw_is_pending(), "a later request must not stay silenced");
}

void
run_gfx_full_redraw_suite(void) {
    RUN_TEST(test_band_force_starts_clear_after_the_fixture_resets_it);
    RUN_TEST(test_band_force_set_and_taken_once);

    RUN_TEST(test_the_request_marks_everything_dirty);
    RUN_TEST(test_the_request_forces_the_next_band_frame);
    RUN_TEST(test_the_request_latches_pending_for_exactly_one_pass);
    RUN_TEST(test_a_request_made_after_consuming_the_last_one_latches_again);
}

SUITE_REGISTER(run_gfx_full_redraw_suite);

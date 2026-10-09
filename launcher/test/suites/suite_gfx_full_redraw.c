/*
 * Portable suite: the two latches behind gfx_request_full_redraw() (gfx_present.h).
 *
 * gfx_full_redraw.h carries no ESP-IDF dependency, the same reason
 * gfx_dirty.h and gfx_present_guard.h do not; see suite_gfx_dirty.c.
 * gfx_present.c's real gfx_request_full_redraw() composes
 * gfx_mark_all_dirty() (dirty_mark_all() here) and gfx_invalidate()
 * (gfx_band_force_all() here) with gfx_full_redraw_latch(); this suite
 * drives that same header state directly, with no panel to satisfy.
 */

#include "suites.h"
#include "unity.h"

#include "gfx_shared_state.h"

#include "gfx/present/gfx_dirty.h"
#include "gfx/present/gfx_full_redraw.h"

static void
fixture(void) {
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

    /* Stands in for shell_apps.c's apply_pending_full_redraw(): the shell reads
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
    gfx_shared_state_t saved;
    gfx_shared_state_save(&saved);
    RUN_TEST(test_band_force_starts_clear_after_the_fixture_resets_it);
    RUN_TEST(test_band_force_set_and_taken_once);

    RUN_TEST(test_the_request_marks_everything_dirty);
    RUN_TEST(test_the_request_forces_the_next_band_frame);
    RUN_TEST(test_the_request_latches_pending_for_exactly_one_pass);
    RUN_TEST(test_a_request_made_after_consuming_the_last_one_latches_again);
    gfx_shared_state_restore(&saved);
}

SUITE_REGISTER(run_gfx_full_redraw_suite);

/*
 * Portable suite: sand_colour_state - the state machine deciding when sand
 * must call gfx_mode_enter()/exit(), pure and host-tested so the crash this
 * guards against (indexed mode still active when the launch menu draws,
 * which has no framebuffer to draw into) can be proven fixed without a
 * device.
 */

#include "suites.h"
#include "unity.h"

#include "apps/sand/sand_colour_state.h"

/* The reported crash's own sequence: start a sim in 256, then return to the
 * launch menu the only real way that happens today - a fresh sand_enter().
 * Indexed mode must not survive it. */
static void
test_entering_sim_in_256_then_returning_to_the_menu_leaves_full(void) {
    sand_colour_state_t st;
    sand_colour_state_init(&st);

    TEST_ASSERT_EQUAL_INT(SAND_GFX_ENTER_INDEXED, sand_colour_on_start_sim(&st, SAND_COLOUR_256));
    TEST_ASSERT_TRUE(sand_colour_indexed_active(&st));

    TEST_ASSERT_EQUAL_INT(SAND_GFX_EXIT_TO_FULL, sand_colour_on_enter_menu(&st));
    TEST_ASSERT_FALSE_MESSAGE(sand_colour_indexed_active(&st),
                              "indexed mode survived a path back to the menu - the menu has no "
                              "framebuffer to draw into");
}

static void
test_entering_sim_in_16_then_returning_to_the_menu_leaves_full(void) {
    sand_colour_state_t st;
    sand_colour_state_init(&st);

    sand_colour_on_start_sim(&st, SAND_COLOUR_16);
    TEST_ASSERT_EQUAL_INT(SAND_GFX_EXIT_TO_FULL, sand_colour_on_enter_menu(&st));
    TEST_ASSERT_FALSE(sand_colour_indexed_active(&st));
}

static void
test_entering_the_menu_while_already_full_asks_for_nothing(void) {
    sand_colour_state_t st;
    sand_colour_state_init(&st);

    sand_colour_on_start_sim(&st, SAND_COLOUR_FULL);
    TEST_ASSERT_EQUAL_INT(SAND_GFX_NONE, sand_colour_on_enter_menu(&st));
    TEST_ASSERT_FALSE(sand_colour_indexed_active(&st));
}

static void
test_leaving_the_app_from_an_indexed_run_restores_full(void) {
    sand_colour_state_t st;
    sand_colour_state_init(&st);

    sand_colour_on_start_sim(&st, SAND_COLOUR_256);
    TEST_ASSERT_EQUAL_INT(SAND_GFX_EXIT_TO_FULL, sand_colour_on_exit_app(&st));
    TEST_ASSERT_FALSE(sand_colour_indexed_active(&st));
}

/* A denied grant (gfx could not allocate the index image) must not leave
 * the state believing indexed mode is running - the menu-safety property
 * above still has to hold for a session that never actually got indexed. */
static void
test_a_failed_grant_leaves_the_state_consistent(void) {
    sand_colour_state_t st;
    sand_colour_state_init(&st);

    sand_colour_on_start_sim(&st, SAND_COLOUR_256);
    sand_colour_grant_failed(&st);
    TEST_ASSERT_FALSE(sand_colour_indexed_active(&st));
    TEST_ASSERT_EQUAL_INT(SAND_GFX_NONE, sand_colour_on_enter_menu(&st));
}

/* The palette/brush screen's own round trip: open suspends (not clears)
 * indexed mode, close restores it - distinct from the menu paths, which
 * clear it outright. */
static void
test_opening_then_closing_an_overlay_round_trips_indexed_mode(void) {
    sand_colour_state_t st;
    sand_colour_state_init(&st);

    sand_colour_on_start_sim(&st, SAND_COLOUR_256);
    TEST_ASSERT_EQUAL_INT(SAND_GFX_EXIT_TO_FULL, sand_colour_on_open_overlay(&st));
    TEST_ASSERT_FALSE(sand_colour_indexed_active(&st));

    TEST_ASSERT_EQUAL_INT(SAND_GFX_ENTER_INDEXED, sand_colour_on_close_overlay(&st));
    TEST_ASSERT_TRUE(sand_colour_indexed_active(&st));
}

/* Closing an overlay that was never suspended (a FULL-mode session opening
 * the palette) asks for nothing - opening it already did, in this state
 * machine, by finding nothing to suspend. */
static void
test_closing_an_overlay_in_full_mode_asks_for_nothing(void) {
    sand_colour_state_t st;
    sand_colour_state_init(&st);

    sand_colour_on_start_sim(&st, SAND_COLOUR_FULL);
    TEST_ASSERT_EQUAL_INT(SAND_GFX_NONE, sand_colour_on_open_overlay(&st));
    TEST_ASSERT_EQUAL_INT(SAND_GFX_NONE, sand_colour_on_close_overlay(&st));
    TEST_ASSERT_FALSE(sand_colour_indexed_active(&st));
}

/* An overlay left suspended (never closed) is still caught: leaving the app
 * or returning to the menu from mid-overlay must not leave indexed_active
 * true just because it was already false at that exact moment - and must
 * clear the suspended flag too, or a later close would wrongly re-enter. */
static void
test_returning_to_the_menu_while_an_overlay_is_suspended_clears_both_flags(void) {
    sand_colour_state_t st;
    sand_colour_state_init(&st);

    sand_colour_on_start_sim(&st, SAND_COLOUR_256);
    sand_colour_on_open_overlay(&st);
    TEST_ASSERT_EQUAL_INT(SAND_GFX_NONE, sand_colour_on_enter_menu(&st));
    TEST_ASSERT_FALSE(sand_colour_indexed_active(&st));

    TEST_ASSERT_EQUAL_INT_MESSAGE(SAND_GFX_NONE, sand_colour_on_close_overlay(&st),
                                  "a suspended-but-abandoned overlay re-entered indexed mode after "
                                  "the menu had already been reached");
}

void
run_sand_colour_state_suite(void) {
    RUN_TEST(test_entering_sim_in_256_then_returning_to_the_menu_leaves_full);
    RUN_TEST(test_entering_sim_in_16_then_returning_to_the_menu_leaves_full);
    RUN_TEST(test_entering_the_menu_while_already_full_asks_for_nothing);
    RUN_TEST(test_leaving_the_app_from_an_indexed_run_restores_full);
    RUN_TEST(test_a_failed_grant_leaves_the_state_consistent);
    RUN_TEST(test_opening_then_closing_an_overlay_round_trips_indexed_mode);
    RUN_TEST(test_closing_an_overlay_in_full_mode_asks_for_nothing);
    RUN_TEST(test_returning_to_the_menu_while_an_overlay_is_suspended_clears_both_flags);
}

SUITE_REGISTER(run_sand_colour_state_suite);

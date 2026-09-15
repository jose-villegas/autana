/*
 * Portable suite: panel_clock, the shell's system panel clock and the
 * switch rule that returns to it.
 */

#include "suites.h"
#include "unity.h"

#include "display/panel_clock.h"

static panel_clock_t pc;

/* The clock gfx is running, as the shell and an app would drive it. */
static int running_hz;

static void
app_starts(void) {
    running_hz = panel_clock_for_switch(&pc);
}

static void
app_exits(void) {
    running_hz = panel_clock_for_switch(&pc);
}

static void
test_nothing_saved_starts_at_the_default(void) {
    panel_clock_init(&pc, false, 0, PANEL_CLOCK_FAST_HZ);
    TEST_ASSERT_EQUAL_INT(PANEL_CLOCK_FAST_HZ, panel_clock_system_hz(&pc));
}

static void
test_a_saved_choice_wins_over_the_default(void) {
    panel_clock_init(&pc, true, PANEL_CLOCK_SLOW_HZ, PANEL_CLOCK_FAST_HZ);
    TEST_ASSERT_EQUAL_INT(PANEL_CLOCK_SLOW_HZ, panel_clock_system_hz(&pc));
}

static void
test_a_saved_rate_the_link_cannot_run_falls_back_to_the_default(void) {
    panel_clock_init(&pc, true, 60 * 1000 * 1000, PANEL_CLOCK_FAST_HZ);
    TEST_ASSERT_EQUAL_INT(PANEL_CLOCK_FAST_HZ, panel_clock_system_hz(&pc));
}

static void
test_an_app_that_forces_nothing_runs_at_the_system_value(void) {
    panel_clock_init(&pc, true, PANEL_CLOCK_SLOW_HZ, PANEL_CLOCK_FAST_HZ);
    running_hz = PANEL_CLOCK_FAST_HZ;
    app_starts();
    TEST_ASSERT_EQUAL_INT(PANEL_CLOCK_SLOW_HZ, running_hz);
}

static void
test_an_app_that_forced_a_clock_and_exited_leaves_the_system_value(void) {
    panel_clock_init(&pc, false, 0, PANEL_CLOCK_FAST_HZ);
    app_starts();
    running_hz = PANEL_CLOCK_SLOW_HZ; /* the app forces its own rate */
    app_exits();
    TEST_ASSERT_EQUAL_INT(PANEL_CLOCK_FAST_HZ, running_hz);
}

static void
test_changing_the_system_value_applies_from_the_next_switch(void) {
    panel_clock_init(&pc, false, 0, PANEL_CLOCK_FAST_HZ);
    TEST_ASSERT_TRUE(panel_clock_set_system(&pc, PANEL_CLOCK_SLOW_HZ));
    app_starts();
    TEST_ASSERT_EQUAL_INT(PANEL_CLOCK_SLOW_HZ, running_hz);

    TEST_ASSERT_FALSE(panel_clock_set_system(&pc, 20 * 1000 * 1000));
    TEST_ASSERT_EQUAL_INT(PANEL_CLOCK_SLOW_HZ, panel_clock_system_hz(&pc));
}

void
run_panel_clock_suite(void) {
    RUN_TEST(test_nothing_saved_starts_at_the_default);
    RUN_TEST(test_a_saved_choice_wins_over_the_default);
    RUN_TEST(test_a_saved_rate_the_link_cannot_run_falls_back_to_the_default);
    RUN_TEST(test_an_app_that_forces_nothing_runs_at_the_system_value);
    RUN_TEST(test_an_app_that_forced_a_clock_and_exited_leaves_the_system_value);
    RUN_TEST(test_changing_the_system_value_applies_from_the_next_switch);
}

SUITE_REGISTER(run_panel_clock_suite);

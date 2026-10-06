/*
 * suite_shell_exit: leaving an app runs its exit, empties the arena and
 * returns to the launcher, in that order. Board only.
 */
#include "suites.h"
#include "unity.h"

#include "app/app_arena.h"

#ifdef DEVICE_BUILD
void shell_test_fixture(void);
bool shell_test_requested_exit(void);
bool shell_test_stale_exit_is_cleared(void);
bool shell_test_leaving_empties_the_arena_after_exit(void);
bool shell_test_band_update_frame_and_present(void);
bool shell_test_leaving_runs_the_systems_app_exit_after_the_apps_exit(void);

/* Every test leaves an app, which empties the arena under the app running
 * the suites. */
static void
fixture(void) {
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, app_arena_mark(), "the app running the suites holds arena memory");
    shell_test_fixture();
}

static void
test_a_requested_exit_leaves_once_and_the_launcher_runs_next(void) {
    fixture();
    TEST_ASSERT_TRUE(shell_test_requested_exit());
}

static void
test_a_request_before_start_does_not_end_the_first_frame(void) {
    fixture();
    TEST_ASSERT_TRUE(shell_test_stale_exit_is_cleared());
}

static void
test_leaving_an_app_empties_the_arena_after_its_exit(void) {
    fixture();
    TEST_ASSERT_TRUE(shell_test_leaving_empties_the_arena_after_exit());
}

static void
test_leaving_runs_the_systems_app_exit_phase_once_after_the_apps_exit(void) {
    fixture();
    TEST_ASSERT_TRUE(shell_test_leaving_runs_the_systems_app_exit_after_the_apps_exit());
}

static void
test_a_band_app_updates_frames_runs_bands_and_closes_the_frame_watch(void) {
    fixture();
    TEST_ASSERT_TRUE(shell_test_band_update_frame_and_present());
}
#endif

void
run_shell_exit_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_a_requested_exit_leaves_once_and_the_launcher_runs_next);
    RUN_TEST(test_a_request_before_start_does_not_end_the_first_frame);
    RUN_TEST(test_leaving_an_app_empties_the_arena_after_its_exit);
    RUN_TEST(test_leaving_runs_the_systems_app_exit_phase_once_after_the_apps_exit);
    RUN_TEST(test_a_band_app_updates_frames_runs_bands_and_closes_the_frame_watch);
#endif
}

SUITE_REGISTER(run_shell_exit_suite);

#include "suites.h"
#include "unity.h"

#include "app_arena.h"

#ifdef DEVICE_BUILD
void shell_test_fixture(void);
bool shell_test_requested_exit(void);
bool shell_test_stale_exit_is_cleared(void);
bool shell_test_every_visit_starts_with_an_empty_arena(void);

/* Every test starts an app, and starting one empties the arena. */
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
test_every_visit_starts_with_an_empty_arena(void) {
    fixture();
    TEST_ASSERT_TRUE(shell_test_every_visit_starts_with_an_empty_arena());
}
#endif

void
run_shell_exit_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_a_requested_exit_leaves_once_and_the_launcher_runs_next);
    RUN_TEST(test_a_request_before_start_does_not_end_the_first_frame);
    RUN_TEST(test_every_visit_starts_with_an_empty_arena);
#endif
}

SUITE_REGISTER(run_shell_exit_suite);

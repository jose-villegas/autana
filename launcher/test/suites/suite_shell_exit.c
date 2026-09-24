#include "suites.h"
#include "unity.h"

#ifdef DEVICE_BUILD
void shell_test_fixture(void);
bool shell_test_requested_exit(void);
bool shell_test_stale_exit_is_cleared(void);

static void
fixture(void) {
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
#endif

void
run_shell_exit_suite(void) {
#ifdef DEVICE_BUILD
    RUN_TEST(test_a_requested_exit_leaves_once_and_the_launcher_runs_next);
    RUN_TEST(test_a_request_before_start_does_not_end_the_first_frame);
#endif
}

SUITE_REGISTER(run_shell_exit_suite);

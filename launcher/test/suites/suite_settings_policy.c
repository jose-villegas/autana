/*
 * Portable suite: settings_policy, when the settings store is erased and
 * started again, over scripted start and erase results.
 */

#include "suites.h"
#include "unity.h"

#include "util/settings_policy.h"

#define FULL   7
#define FORMAT 8
#define OTHER  9

static int init_results[2];
static int init_calls;
static int erase_result;
static int erase_calls;

static int
scripted_init(void) {
    return init_results[init_calls++ < 1 ? 0 : 1];
}

static int
scripted_erase(void) {
    erase_calls++;
    return erase_result;
}

static int
start(int first, int second, int erase) {
    init_results[0] = first;
    init_results[1] = second;
    init_calls = 0;
    erase_calls = 0;
    erase_result = erase;
    const settings_store_ops_t ops = {scripted_init, scripted_erase, FULL, FORMAT};
    return settings_store_start(&ops);
}

static void
test_a_store_that_starts_is_left_alone(void) {
    TEST_ASSERT_EQUAL_INT(0, start(0, 0, 0));
    TEST_ASSERT_EQUAL_INT(1, init_calls);
    TEST_ASSERT_EQUAL_INT(0, erase_calls);
}

static void
test_a_full_store_is_erased_and_started_again(void) {
    TEST_ASSERT_EQUAL_INT(0, start(FULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(1, erase_calls);
    TEST_ASSERT_EQUAL_INT(2, init_calls);
}

static void
test_a_store_in_a_newer_format_is_erased_and_started_again(void) {
    TEST_ASSERT_EQUAL_INT(0, start(FORMAT, 0, 0));
    TEST_ASSERT_EQUAL_INT(1, erase_calls);
    TEST_ASSERT_EQUAL_INT(2, init_calls);
}

static void
test_any_other_failure_is_reported_without_an_erase(void) {
    TEST_ASSERT_EQUAL_INT(OTHER, start(OTHER, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, erase_calls);
    TEST_ASSERT_EQUAL_INT(1, init_calls);
}

static void
test_a_failed_erase_is_reported_and_init_is_not_retried(void) {
    TEST_ASSERT_EQUAL_INT(OTHER, start(FULL, 0, OTHER));
    TEST_ASSERT_EQUAL_INT(1, erase_calls);
    TEST_ASSERT_EQUAL_INT(1, init_calls);
}

static void
test_a_store_still_stale_after_one_erase_is_not_erased_again(void) {
    TEST_ASSERT_EQUAL_INT(FULL, start(FULL, FULL, 0));
    TEST_ASSERT_EQUAL_INT(1, erase_calls);
    TEST_ASSERT_EQUAL_INT(2, init_calls);
}

void
run_settings_policy_suite(void) {
    RUN_TEST(test_a_store_that_starts_is_left_alone);
    RUN_TEST(test_a_full_store_is_erased_and_started_again);
    RUN_TEST(test_a_store_in_a_newer_format_is_erased_and_started_again);
    RUN_TEST(test_any_other_failure_is_reported_without_an_erase);
    RUN_TEST(test_a_failed_erase_is_reported_and_init_is_not_retried);
    RUN_TEST(test_a_store_still_stale_after_one_erase_is_not_erased_again);
}

SUITE_REGISTER(run_settings_policy_suite);

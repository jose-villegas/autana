/*
 * Portable suite: settings_policy, when the settings store is erased and
 * started again, over scripted start and erase results.
 */

#include <stdlib.h>

#include "suites.h"
#include "test_cleanup.h"
#include "unity.h"

#include "util/settings_policy.h"

#define FULL   7
#define FORMAT 8
#define OTHER  9

typedef struct {
    int init_results[2];
    int init_calls;
    int erase_result;
    int erase_calls;
} script_t;

/* Allocated per test: a static would be internal RAM the device image keeps. */
static script_t* script;

static int
scripted_init(void) {
    return script->init_results[script->init_calls++ < 1 ? 0 : 1];
}

static int
scripted_erase(void) {
    script->erase_calls++;
    return script->erase_result;
}

static void
end_script(void) {
    free(script);
    script = NULL;
}

static int
start(int first, int second, int erase) {
    script = calloc(1, sizeof *script);
    TEST_ASSERT_NOT_NULL(script);
    suite_set_test_cleanup(end_script);
    script->init_results[0] = first;
    script->init_results[1] = second;
    script->erase_result = erase;
    const settings_store_ops_t ops = {scripted_init, scripted_erase, FULL, FORMAT};
    return settings_store_start(&ops);
}

static void
test_a_store_that_starts_is_left_alone(void) {
    TEST_ASSERT_EQUAL_INT(0, start(0, 0, 0));
    TEST_ASSERT_EQUAL_INT(1, script->init_calls);
    TEST_ASSERT_EQUAL_INT(0, script->erase_calls);
}

static void
test_a_full_store_is_erased_and_started_again(void) {
    TEST_ASSERT_EQUAL_INT(0, start(FULL, 0, 0));
    TEST_ASSERT_EQUAL_INT(1, script->erase_calls);
    TEST_ASSERT_EQUAL_INT(2, script->init_calls);
}

static void
test_a_store_in_a_newer_format_is_erased_and_started_again(void) {
    TEST_ASSERT_EQUAL_INT(0, start(FORMAT, 0, 0));
    TEST_ASSERT_EQUAL_INT(1, script->erase_calls);
    TEST_ASSERT_EQUAL_INT(2, script->init_calls);
}

static void
test_any_other_failure_is_reported_without_an_erase(void) {
    TEST_ASSERT_EQUAL_INT(OTHER, start(OTHER, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, script->erase_calls);
    TEST_ASSERT_EQUAL_INT(1, script->init_calls);
}

static void
test_a_failed_erase_is_reported_and_init_is_not_retried(void) {
    TEST_ASSERT_EQUAL_INT(OTHER, start(FULL, 0, OTHER));
    TEST_ASSERT_EQUAL_INT(1, script->erase_calls);
    TEST_ASSERT_EQUAL_INT(1, script->init_calls);
}

static void
test_a_store_still_stale_after_one_erase_is_not_erased_again(void) {
    TEST_ASSERT_EQUAL_INT(FULL, start(FULL, FULL, 0));
    TEST_ASSERT_EQUAL_INT(1, script->erase_calls);
    TEST_ASSERT_EQUAL_INT(2, script->init_calls);
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

/*
 * Portable suite: narrowing one suite's run to the tests whose name contains
 * a pattern (suites.h). The narrowed suite is a fixture this file registers
 * on request; its "tests" ask the gate directly instead of going through
 * RUN_TEST, since a real RUN_TEST would nest one Unity run inside another.
 */

#include <string.h>

#include "suites.h"
#include "timing.h"
#include "unity.h"

#define FIXTURE_TESTS 4

static const char* const fixture_names[FIXTURE_TESTS] = {"test_fire_fits", "test_gas_fits", "test_fire_and_gas_fit",
                                                         "test_water_fits"};

static int ran[FIXTURE_TESTS];
static int probe_calls;

static void
probe(void) {
    probe_calls++;
}

static void
filter_fixture_suite(void) {
    for (int i = 0; i < FIXTURE_TESTS; i++) {
        ran[i] += suites_test_runs(fixture_names[i]) ? 1 : 0;
    }
}

static void
timed_probe_suite(void) {
    suite_run_test_timed(probe, "test_unselected_probe", 0);
}

SUITE_REGISTER_ON_REQUEST(filter_fixture_suite);
SUITE_REGISTER_ON_REQUEST(timed_probe_suite);

static void
fixture(void) {
    suites_filter_clear();
    memset(ran, 0, sizeof ran);
    probe_calls = 0;
}

static void
test_without_a_filter_every_test_runs(void) {
    fixture();
    TEST_ASSERT_TRUE(suites_run_one("filter_fixture_suite"));
    for (int i = 0; i < FIXTURE_TESTS; i++) {
        TEST_ASSERT_EQUAL_INT(1, ran[i]);
    }
    TEST_ASSERT_EQUAL_INT(FIXTURE_TESTS, suites_filter_selected());
}

static void
test_a_pattern_runs_only_the_tests_whose_name_contains_it(void) {
    fixture();
    TEST_ASSERT_TRUE(suites_filter_add("fire"));
    TEST_ASSERT_TRUE(suites_run_one("filter_fixture_suite"));
    TEST_ASSERT_EQUAL_INT(1, ran[0]);
    TEST_ASSERT_EQUAL_INT(0, ran[1]);
    TEST_ASSERT_EQUAL_INT(1, ran[2]);
    TEST_ASSERT_EQUAL_INT(0, ran[3]);
    TEST_ASSERT_EQUAL_INT(2, suites_filter_selected());
    TEST_ASSERT_EQUAL_INT(0, suites_filter_unmatched());
    suites_filter_clear();
}

static void
test_several_patterns_select_the_union(void) {
    fixture();
    TEST_ASSERT_TRUE(suites_filter_add("fire_fits"));
    TEST_ASSERT_TRUE(suites_filter_add("water"));
    TEST_ASSERT_TRUE(suites_run_one("filter_fixture_suite"));
    TEST_ASSERT_EQUAL_INT(1, ran[0]);
    TEST_ASSERT_EQUAL_INT(0, ran[1]);
    TEST_ASSERT_EQUAL_INT(0, ran[2]);
    TEST_ASSERT_EQUAL_INT(1, ran[3]);
    suites_filter_clear();
}

static void
test_a_pattern_matching_nothing_runs_no_test_at_all(void) {
    fixture();
    TEST_ASSERT_TRUE(suites_filter_add("fire"));
    TEST_ASSERT_TRUE(suites_filter_add("nothing_is_called_this"));
    TEST_ASSERT_TRUE(suites_run_one("filter_fixture_suite"));
    for (int i = 0; i < FIXTURE_TESTS; i++) {
        TEST_ASSERT_EQUAL_INT(0, ran[i]);
    }
    TEST_ASSERT_EQUAL_INT(1, suites_filter_unmatched());
    TEST_ASSERT_EQUAL_INT(0, suites_filter_selected());
    suites_filter_clear();
}

static void
test_the_timed_runner_skips_a_test_the_filter_does_not_select(void) {
    fixture();
    TEST_ASSERT_TRUE(suites_filter_add("nothing_is_called_this"));
    TEST_ASSERT_TRUE(suites_run_one("timed_probe_suite"));
    TEST_ASSERT_EQUAL_INT(0, probe_calls);
    suites_filter_clear();
}

static void
test_clearing_the_filter_restores_the_whole_suite(void) {
    fixture();
    TEST_ASSERT_TRUE(suites_filter_add("fire"));
    suites_filter_clear();
    TEST_ASSERT_TRUE(suites_run_one("filter_fixture_suite"));
    TEST_ASSERT_EQUAL_INT(FIXTURE_TESTS, suites_filter_selected());
}

static void
test_a_pattern_the_device_cannot_hold_is_refused(void) {
    fixture();
    char too_long[SUITE_FILTER_LEN + 1];
    memset(too_long, 'x', sizeof too_long - 1);
    too_long[sizeof too_long - 1] = '\0';
    TEST_ASSERT_FALSE(suites_filter_add(""));
    TEST_ASSERT_FALSE(suites_filter_add(too_long));
    for (int i = 0; i < SUITE_FILTER_MAX; i++) {
        TEST_ASSERT_TRUE(suites_filter_add("fire"));
    }
    TEST_ASSERT_FALSE(suites_filter_add("fire"));
    suites_filter_clear();
}

void
run_suite_filter_suite(void) {
    RUN_TEST(test_without_a_filter_every_test_runs);
    RUN_TEST(test_a_pattern_runs_only_the_tests_whose_name_contains_it);
    RUN_TEST(test_several_patterns_select_the_union);
    RUN_TEST(test_a_pattern_matching_nothing_runs_no_test_at_all);
    RUN_TEST(test_the_timed_runner_skips_a_test_the_filter_does_not_select);
    RUN_TEST(test_clearing_the_filter_restores_the_whole_suite);
    RUN_TEST(test_a_pattern_the_device_cannot_hold_is_refused);
}

SUITE_REGISTER(run_suite_filter_suite)

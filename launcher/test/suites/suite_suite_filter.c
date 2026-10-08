/*
 * Portable suite: narrowing a suite's run to the tests whose name contains a
 * pattern (suites.h). Host only, so the fixture suites are not on the board.
 * gate_fixture_suite asks the gate directly, since a real RUN_TEST would
 * nest one Unity run in another; filter_fixture_suite uses real ones for
 * `host_tests --run` (launcher/test/tests/test_suite_filter_output.py).
 */
#ifndef DEVICE_BUILD

#include <stdio.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#define GATE_TESTS 4

static const char* const gate_names[GATE_TESTS] = {"test_fire_fits", "test_gas_fits", "test_fire_and_gas_fit",
                                                   "test_water_fits"};

static int ran[GATE_TESTS];
static int probe_calls;

static void
probe(void) {
    probe_calls++;
}

static void
gate_fixture_suite(void) {
    for (int i = 0; i < GATE_TESTS; i++) {
        ran[i] += suites_test_runs(gate_names[i]) ? 1 : 0;
    }
}

static void
timed_probe_suite(void) {
    suite_run_test_timed(probe, "test_unselected_probe", 0);
}

static void
test_fire_fits(void) {
    printf("PROBE_RAN test_fire_fits\n");
}

static void
test_gas_fits(void) {
    printf("PROBE_RAN test_gas_fits\n");
}

static void
test_water_fits(void) {
    printf("PROBE_RAN test_water_fits\n");
}

static void
filter_fixture_suite(void) {
    RUN_TEST(test_fire_fits);
    RUN_TEST(test_gas_fits);
    RUN_TEST(test_water_fits);
}

SUITE_REGISTER_ON_REQUEST(gate_fixture_suite);
SUITE_REGISTER_ON_REQUEST(timed_probe_suite);
SUITE_REGISTER_ON_REQUEST(filter_fixture_suite);

static void
fixture(void) {
    memset(ran, 0, sizeof ran);
    probe_calls = 0;
}

static suite_run_t
request(const char* text) {
    suite_run_t run;
    suites_run_request(text, &run);
    return run;
}

static void
test_a_plain_request_runs_every_test(void) {
    fixture();
    const suite_run_t run = request("gate_fixture_suite");
    TEST_ASSERT_TRUE(run.found);
    for (int i = 0; i < GATE_TESTS; i++) {
        TEST_ASSERT_EQUAL_INT(1, ran[i]);
    }
    TEST_ASSERT_EQUAL_INT(GATE_TESTS, run.selected);
    TEST_ASSERT_EQUAL_INT(0, run.unmatched);
}

static void
test_a_pattern_runs_only_the_tests_whose_name_contains_it(void) {
    fixture();
    const suite_run_t run = request("gate_fixture_suite fire");
    TEST_ASSERT_EQUAL_INT(1, ran[0]);
    TEST_ASSERT_EQUAL_INT(0, ran[1]);
    TEST_ASSERT_EQUAL_INT(1, ran[2]);
    TEST_ASSERT_EQUAL_INT(0, ran[3]);
    TEST_ASSERT_EQUAL_INT(2, run.selected);
    TEST_ASSERT_EQUAL_INT(0, run.unmatched);
}

static void
test_several_patterns_select_the_union(void) {
    fixture();
    const suite_run_t run = request("gate_fixture_suite fire_fits,water");
    TEST_ASSERT_EQUAL_INT(1, ran[0]);
    TEST_ASSERT_EQUAL_INT(0, ran[1]);
    TEST_ASSERT_EQUAL_INT(0, ran[2]);
    TEST_ASSERT_EQUAL_INT(1, ran[3]);
    TEST_ASSERT_EQUAL_INT(2, run.selected);
}

static void
test_a_lone_pattern_matching_nothing_runs_no_test(void) {
    fixture();
    const suite_run_t run = request("gate_fixture_suite nothing_is_called_this");
    for (int i = 0; i < GATE_TESTS; i++) {
        TEST_ASSERT_EQUAL_INT(0, ran[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, run.selected);
    TEST_ASSERT_EQUAL_INT(1, run.unmatched);
    TEST_ASSERT_FALSE(run.refused);
}

static void
test_a_miss_beside_a_hit_still_runs_the_hit_and_is_counted(void) {
    fixture();
    const suite_run_t run = request("gate_fixture_suite fire,nothing_is_called_this");
    TEST_ASSERT_EQUAL_INT(2, run.selected);
    TEST_ASSERT_EQUAL_INT(1, run.unmatched);
}

static void
test_a_plain_request_after_a_filtered_one_runs_every_test(void) {
    fixture();
    (void)request("gate_fixture_suite fire");
    memset(ran, 0, sizeof ran);
    const suite_run_t run = request("gate_fixture_suite");
    for (int i = 0; i < GATE_TESTS; i++) {
        TEST_ASSERT_EQUAL_INT(1, ran[i]);
    }
    TEST_ASSERT_EQUAL_INT(GATE_TESTS, run.selected);
}

static void
test_the_timed_runner_skips_a_test_the_filter_does_not_select(void) {
    fixture();
    (void)request("timed_probe_suite nothing_is_called_this");
    TEST_ASSERT_EQUAL_INT(0, probe_calls);
}

/* Repeats "x" until `length` characters, as one pattern. */
static void
pattern_of(char* out, int length) {
    memset(out, 'x', (size_t)length);
    out[length] = '\0';
}

static suite_run_t
request_with(const char* pattern, int copies) {
    char text[SUITE_NAME_MAX + 2 + SUITE_FILTER_MAX * SUITE_FILTER_LEN + SUITE_FILTER_LEN];
    int n = snprintf(text, sizeof text, "gate_fixture_suite ");
    for (int i = 0; i < copies; i++) {
        n += snprintf(text + n, sizeof text - (size_t)n, "%s%s", i > 0 ? "," : "", pattern);
    }
    return request(text);
}

static void
test_a_pattern_at_the_length_limit_is_accepted_and_one_more_is_refused(void) {
    fixture();
    char pattern[SUITE_FILTER_LEN + 1];
    pattern_of(pattern, SUITE_FILTER_LEN - 1);
    TEST_ASSERT_FALSE(request_with(pattern, 1).refused);
    pattern_of(pattern, SUITE_FILTER_LEN);
    const suite_run_t run = request_with(pattern, 1);
    TEST_ASSERT_TRUE(run.refused);
    TEST_ASSERT_EQUAL_INT(0, run.selected);
}

static void
test_the_pattern_count_at_the_limit_is_accepted_and_one_more_is_refused(void) {
    fixture();
    TEST_ASSERT_FALSE(request_with("fire", SUITE_FILTER_MAX).refused);
    fixture();
    const suite_run_t run = request_with("fire", SUITE_FILTER_MAX + 1);
    TEST_ASSERT_TRUE(run.refused);
    for (int i = 0; i < GATE_TESTS; i++) {
        TEST_ASSERT_EQUAL_INT(0, ran[i]);
    }
}

static void
test_an_empty_pattern_is_refused(void) {
    fixture();
    TEST_ASSERT_TRUE(request("gate_fixture_suite fire,,gas").refused);
}

static void
test_an_unknown_or_overlong_suite_is_not_found(void) {
    fixture();
    TEST_ASSERT_FALSE(request("no_such_suite fire").found);
    char name[SUITE_NAME_MAX + 3];
    pattern_of(name, SUITE_NAME_MAX + 1);
    TEST_ASSERT_FALSE(request(name).found);
}

void
run_suite_filter_suite(void) {
    RUN_TEST(test_a_plain_request_runs_every_test);
    RUN_TEST(test_a_pattern_runs_only_the_tests_whose_name_contains_it);
    RUN_TEST(test_several_patterns_select_the_union);
    RUN_TEST(test_a_lone_pattern_matching_nothing_runs_no_test);
    RUN_TEST(test_a_miss_beside_a_hit_still_runs_the_hit_and_is_counted);
    RUN_TEST(test_a_plain_request_after_a_filtered_one_runs_every_test);
    RUN_TEST(test_the_timed_runner_skips_a_test_the_filter_does_not_select);
    RUN_TEST(test_a_pattern_at_the_length_limit_is_accepted_and_one_more_is_refused);
    RUN_TEST(test_the_pattern_count_at_the_limit_is_accepted_and_one_more_is_refused);
    RUN_TEST(test_an_empty_pattern_is_refused);
    RUN_TEST(test_an_unknown_or_overlong_suite_is_not_found);
}

SUITE_REGISTER(run_suite_filter_suite)

#endif /* DEVICE_BUILD */

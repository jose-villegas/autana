/*
 * Portable suite: narrowing a suite's run to the tests a pattern picks
 * (suites.h). Host only. gate_fixture_suite asks the gate directly, since a
 * real RUN_TEST would nest one Unity run in another; the other fixtures run
 * real ones for launcher/test/tests/test_suite_filter_output.py.
 */
#ifndef DEVICE_BUILD

#include <stdio.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "collections/strbuf.h"

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

static const char* const case_scenes[] = {"sponza-fitted", "sponza-fitted-full"};
static const char* const case_quarters[] = {"portrait", "landscape"};

static void
test_case(void) {
    printf("PROBE_RAN %s\n", Unity.CurrentTestName);
}

/* A table-driven loop as the perf suites run one, with a level some rows
 * leave out. */
static void
case_fixture_suite(void) {
    for (size_t scene = 0; scene < sizeof case_scenes / sizeof case_scenes[0]; scene++) {
        for (size_t quarter = 0; quarter < sizeof case_quarters / sizeof case_quarters[0]; quarter++) {
            RUN_TEST_CASE(test_case, case_scenes[scene], NULL, case_quarters[quarter]);
        }
    }
    RUN_TEST_CASE(test_case, "capybara", "", "front", "portrait");
}

/* A case named exactly SUITE_TEST_NAME_MAX characters, then one past it. */
static void
case_limit_suite(void) {
    char segment[SUITE_TEST_NAME_MAX + 1];
    const size_t at_limit = SUITE_TEST_NAME_MAX - sizeof "test_case";
    memset(segment, 'x', at_limit + 1);
    segment[at_limit] = '\0';
    RUN_TEST_CASE(test_case, segment);
    segment[at_limit] = 'x';
    segment[at_limit + 1] = '\0';
    RUN_TEST_CASE(test_case, segment);
}

SUITE_REGISTER_ON_REQUEST(gate_fixture_suite);
SUITE_REGISTER_ON_REQUEST(timed_probe_suite);
SUITE_REGISTER_ON_REQUEST(filter_fixture_suite);
SUITE_REGISTER_ON_REQUEST(case_fixture_suite);
SUITE_REGISTER_ON_REQUEST(case_limit_suite);

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
    strbuf_t line = strbuf_make(text, sizeof text);
    strbuf_appendf(&line, "gate_fixture_suite ");
    for (int i = 0; i < copies; i++) {
        TEST_ASSERT_TRUE(strbuf_appendf(&line, "%s%s", i > 0 ? "," : "", pattern));
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

typedef struct {
    const char* pattern;
    const char* name;
    bool picks;
} filter_case_t;

static const filter_case_t filter_cases[] = {
#define SUITE_FILTER_CASE(pattern, name, picks) {pattern, name, picks},
#include "suite_filter_cases.inc"
#undef SUITE_FILTER_CASE
};

static void
test_each_pattern_picks_exactly_the_names_its_case_says(void) {
    for (size_t i = 0; i < sizeof filter_cases / sizeof filter_cases[0]; i++) {
        const filter_case_t* c = &filter_cases[i];
        char message[SUITE_FILTER_LEN + SUITE_TEST_NAME_MAX + sizeof " on "];
        strbuf_t text = strbuf_make(message, sizeof message);
        strbuf_appendf(&text, "%s on %s", c->pattern, c->name);
        TEST_ASSERT_TRUE_MESSAGE(c->picks == suite_test_matches(c->pattern, c->name), message);
    }
}

typedef struct {
    int seen;
    int fixtures_on_request;
    bool in_order;
    const char* previous;
} listing_t;

static void
list_one(const char* name, bool on_request, void* context) {
    listing_t* listing = context;
    listing->seen++;
    listing->in_order &= listing->previous == NULL || strcmp(listing->previous, name) < 0;
    listing->previous = name;
    listing->fixtures_on_request += on_request && strstr(name, "_fixture_suite") != NULL;
}

static void
test_the_listing_names_every_suite_in_order_with_its_on_request_mark(void) {
    listing_t listing = {.in_order = true};
    suites_each(list_one, &listing);
    TEST_ASSERT_TRUE(listing.in_order);
    TEST_ASSERT_GREATER_THAN_INT(2, listing.seen);
    TEST_ASSERT_EQUAL_INT(3, listing.fixtures_on_request);
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
    RUN_TEST(test_each_pattern_picks_exactly_the_names_its_case_says);
    RUN_TEST(test_the_listing_names_every_suite_in_order_with_its_on_request_mark);
}

SUITE_REGISTER(run_suite_filter_suite)

#endif /* DEVICE_BUILD */

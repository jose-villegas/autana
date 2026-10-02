/*
 * Host-only suite: the shell's system registry runs every phase over the
 * registered systems in `order`, then by name, hands each the time it was
 * given, and skips a system that leaves a phase NULL. Its fake systems exist
 * only inside each test; the real list is put back even after a failure.
 */

#include "suites.h"

#ifndef DEVICE_BUILD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "shell/shell_system.h"

#define CALLS_MAX   32
#define CALL_LEN    32
#define SYSTEMS_MAX 3

/* The running test's own buffers, freed by the next fixture() if a failed
 * assertion skipped the test's end. */
static char (*calls)[CALL_LEN];
static int call_count;
static shell_system_t* systems;

/* The real list, held while a test runs its own. */
static shell_system_t* real_list;
static bool swapped;

static void
restore(void) {
    if (swapped) {
        shell_system_swap_for_test(real_list);
        swapped = false;
    }
    free(calls);
    free(systems);
    calls = NULL;
    systems = NULL;
}

static void
fixture(void) {
    restore();
    calls = malloc(sizeof(*calls) * CALLS_MAX);
    systems = calloc(SYSTEMS_MAX, sizeof(*systems));
    TEST_ASSERT_NOT_NULL(calls);
    TEST_ASSERT_NOT_NULL(systems);
    call_count = 0;
    real_list = shell_system_swap_for_test(NULL);
    swapped = true;
}

static void
note(const char* system, const char* phase, uint32_t dt_ms) {
    TEST_ASSERT_LESS_THAN_INT(CALLS_MAX, call_count);
    snprintf(calls[call_count++], CALL_LEN, "%s.%s@%u", system, phase, (unsigned)dt_ms);
}

#define FAKE_SYSTEM(id)                                                                                                \
    static void id##_update(uint32_t dt_ms) { note(#id, "update", dt_ms); }                                            \
    static void id##_compose(uint32_t dt_ms) { note(#id, "compose", dt_ms); }                                          \
    static void id##_app_exit(void) { note(#id, "app_exit", 0); }

FAKE_SYSTEM(a)
FAKE_SYSTEM(b)
FAKE_SYSTEM(c)

/* A fake that takes part in every phase, under `name`. */
static shell_system_t
fake(const char* name, int order) {
    switch (name[0]) {
        case 'a':
            return (shell_system_t){
                .name = name, .order = order, .update = a_update, .compose = a_compose, .app_exit = a_app_exit};
        case 'b':
            return (shell_system_t){
                .name = name, .order = order, .update = b_update, .compose = b_compose, .app_exit = b_app_exit};
        default:
            return (shell_system_t){
                .name = name, .order = order, .update = c_update, .compose = c_compose, .app_exit = c_app_exit};
    }
}

static bool
overlapping(void) {
    return true;
}

static bool
not_overlapping(void) {
    return false;
}

typedef struct {
    const char* phase;
    void (*run)(uint32_t dt_ms);
} phase_t;

static void
run_app_exit(uint32_t dt_ms) {
    (void)dt_ms;
    shell_systems_app_exit();
}

static const phase_t phases[] = {
    {"update", shell_systems_update},
    {"compose", shell_systems_compose},
    {"app_exit", run_app_exit},
};
#define PHASE_COUNT ((int)(sizeof phases / sizeof phases[0]))

/* The call a fake makes for `phase`: app_exit is given no time. */
static void
expected_call(char* out, const char* system, const char* phase, uint32_t dt_ms) {
    snprintf(out, CALL_LEN, "%s.%s@%u", system, phase, strcmp(phase, "app_exit") == 0 ? 0u : (unsigned)dt_ms);
}

static void
expect_calls(const char* what, const char* const* names, int count, const phase_t* phase, uint32_t dt_ms) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(count, call_count, what);
    for (int i = 0; i < count; i++) {
        char expected[CALL_LEN];
        expected_call(expected, names[i], phase->phase, dt_ms);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(expected, calls[i], what);
    }
}

static void
test_every_phase_runs_the_systems_by_order_whatever_order_they_registered_in(void) {
    fixture();
    systems[0] = fake("c", 30);
    systems[1] = fake("a", 10);
    systems[2] = fake("b", 20);
    for (int i = 0; i < SYSTEMS_MAX; i++) {
        shell_system_register(&systems[i]);
    }

    static const char* const by_order[] = {"a", "b", "c"};
    for (int p = 0; p < PHASE_COUNT; p++) {
        call_count = 0;
        phases[p].run(16);
        expect_calls(phases[p].phase, by_order, 3, &phases[p], 16);
    }
    restore();
}

static void
test_systems_with_the_same_order_run_by_name_whichever_registered_first(void) {
    static const struct {
        const char* case_name;
        const char* first;
        const char* second;
    } cases[] = {
        {"registered in name order", "a", "b"},
        {"registered against name order", "b", "a"},
    };

    static const char* const by_name[] = {"a", "b"};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fixture();
        systems[0] = fake(cases[i].first, 50);
        systems[1] = fake(cases[i].second, 50);
        shell_system_register(&systems[0]);
        shell_system_register(&systems[1]);
        for (int p = 0; p < PHASE_COUNT; p++) {
            call_count = 0;
            phases[p].run(16);
            expect_calls(cases[i].case_name, by_name, 2, &phases[p], 16);
        }
        restore();
    }
}

static void
test_each_system_is_given_the_passes_own_time(void) {
    static const uint32_t dts[] = {1, 250};
    static const char* const both[] = {"a", "b"};
    for (size_t i = 0; i < sizeof dts / sizeof dts[0]; i++) {
        fixture();
        systems[0] = fake("a", 10);
        systems[1] = fake("b", 20);
        shell_system_register(&systems[0]);
        shell_system_register(&systems[1]);
        for (int p = 0; p < 2; p++) { /* the two phases that take time */
            call_count = 0;
            phases[p].run(dts[i]);
            expect_calls(phases[p].phase, both, 2, &phases[p], dts[i]);
        }
        restore();
    }
}

static void
test_a_system_whose_phase_is_null_is_skipped_by_that_phase_alone(void) {
    fixture();
    /* a draws only; b only frees. */
    systems[0] = (shell_system_t){.name = "a", .order = 10, .update = a_update, .compose = a_compose};
    systems[1] = (shell_system_t){.name = "b", .order = 20, .app_exit = b_app_exit};
    shell_system_register(&systems[1]);
    shell_system_register(&systems[0]);

    static const char* const runs[PHASE_COUNT] = {"a", "a", "b"};
    for (int p = 0; p < PHASE_COUNT; p++) {
        call_count = 0;
        phases[p].run(16);
        expect_calls(phases[p].phase, &runs[p], 1, &phases[p], 16);
    }
    restore();
}

static void
test_the_present_overlaps_when_any_system_asks_and_never_by_default(void) {
    static const struct {
        const char* name;
        bool (*first)(void);
        bool (*second)(void);
        bool overlaps;
    } cases[] = {
        {"no system asks", NULL, NULL, false},
        {"both decline", not_overlapping, not_overlapping, false},
        {"the later one asks", not_overlapping, overlapping, true},
        {"the earlier one asks", overlapping, NULL, true},
    };

    fixture();
    TEST_ASSERT_FALSE_MESSAGE(shell_systems_overlap_present(), "an empty registry");
    restore();

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fixture();
        systems[0] = (shell_system_t){.name = "a", .order = 10, .overlaps_present = cases[i].first};
        systems[1] = (shell_system_t){.name = "b", .order = 20, .overlaps_present = cases[i].second};
        shell_system_register(&systems[0]);
        shell_system_register(&systems[1]);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].overlaps, shell_systems_overlap_present(), cases[i].name);
        restore();
    }
}

void
run_shell_system_suite(void) {
    RUN_TEST(test_every_phase_runs_the_systems_by_order_whatever_order_they_registered_in);
    RUN_TEST(test_systems_with_the_same_order_run_by_name_whichever_registered_first);
    RUN_TEST(test_each_system_is_given_the_passes_own_time);
    RUN_TEST(test_a_system_whose_phase_is_null_is_skipped_by_that_phase_alone);
    RUN_TEST(test_the_present_overlaps_when_any_system_asks_and_never_by_default);
    restore();
}

#else

void
run_shell_system_suite(void) {}

#endif

SUITE_REGISTER(run_shell_system_suite);

/*
 * Host-only suite: the shell's system registry runs every phase over the
 * registered systems in `order`, and skips a system that leaves a phase
 * NULL. Its fake systems exist only inside each test; the real list is put
 * back afterwards.
 */

#include "suites.h"

#ifndef DEVICE_BUILD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "shell/shell_system.h"

#define CALLS_MAX 32
#define CALL_LEN  24

/* Where the fakes write; a buffer of the running test's own. */
static char (*calls)[CALL_LEN];
static int call_count;

static void
note(const char* system, const char* phase) {
    TEST_ASSERT_LESS_THAN_INT(CALLS_MAX, call_count);
    snprintf(calls[call_count++], CALL_LEN, "%s.%s", system, phase);
}

#define FAKE_SYSTEM(id)                                                                                                \
    static void id##_update(uint32_t dt_ms) {                                                                          \
        (void)dt_ms;                                                                                                   \
        note(#id, "update");                                                                                           \
    }                                                                                                                  \
    static void id##_compose(uint32_t dt_ms) {                                                                         \
        (void)dt_ms;                                                                                                   \
        note(#id, "compose");                                                                                          \
    }                                                                                                                  \
    static void id##_overlay(void) { note(#id, "overlay"); }                                                           \
    static void id##_invalidate(void) { note(#id, "invalidate"); }                                                     \
    static void id##_app_exit(void) { note(#id, "app_exit"); }

FAKE_SYSTEM(a)
FAKE_SYSTEM(b)
FAKE_SYSTEM(c)

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
    void (*run)(void);
} phase_t;

static void
run_update(void) {
    shell_systems_update(16);
}

static void
run_compose(void) {
    shell_systems_compose(16);
}

static const phase_t phases[] = {
    {"update", run_update},
    {"compose", run_compose},
    {"overlay", shell_systems_overlay},
    {"invalidate", shell_systems_invalidate},
    {"app_exit", shell_systems_app_exit},
};
#define PHASE_COUNT ((int)(sizeof phases / sizeof phases[0]))

static shell_system_t*
fake(shell_system_t* storage, const char* name, int order) {
    if (strcmp(name, "a") == 0) {
        *storage = (shell_system_t){name, order, a_update, a_compose, a_overlay, a_invalidate, a_app_exit, NULL, NULL};
    } else if (strcmp(name, "b") == 0) {
        *storage = (shell_system_t){name, order, b_update, b_compose, b_overlay, b_invalidate, b_app_exit, NULL, NULL};
    } else {
        *storage = (shell_system_t){name, order, c_update, c_compose, c_overlay, c_invalidate, c_app_exit, NULL, NULL};
    }
    return storage;
}

static void
expect_calls(const char* phase, const char* const* expected, int count) {
    char message[64];
    snprintf(message, sizeof message, "phase %s", phase);
    TEST_ASSERT_EQUAL_INT_MESSAGE(count, call_count, message);
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(expected[i], calls[i], message);
    }
}

static void
test_every_phase_runs_the_systems_by_order_whatever_order_they_registered_in(void) {
    calls = malloc(sizeof(*calls) * CALLS_MAX);
    shell_system_t* systems = malloc(sizeof(*systems) * 3);
    TEST_ASSERT_NOT_NULL(calls);
    TEST_ASSERT_NOT_NULL(systems);
    shell_system_t* real = shell_system_swap_for_test(NULL);

    shell_system_register(fake(&systems[0], "c", 30));
    shell_system_register(fake(&systems[1], "a", 10));
    shell_system_register(fake(&systems[2], "b", 20));

    for (int p = 0; p < PHASE_COUNT; p++) {
        char expected[3][CALL_LEN];
        const char* rows[3];
        const char* names[3] = {"a", "b", "c"};
        for (int i = 0; i < 3; i++) {
            snprintf(expected[i], CALL_LEN, "%s.%s", names[i], phases[p].phase);
            rows[i] = expected[i];
        }
        call_count = 0;
        phases[p].run();
        expect_calls(phases[p].phase, rows, 3);
    }

    shell_system_swap_for_test(real);
    free(systems);
    free(calls);
}

static void
test_a_system_whose_phase_is_null_is_skipped_by_that_phase_alone(void) {
    calls = malloc(sizeof(*calls) * CALLS_MAX);
    shell_system_t* systems = malloc(sizeof(*systems) * 2);
    TEST_ASSERT_NOT_NULL(calls);
    TEST_ASSERT_NOT_NULL(systems);
    shell_system_t* real = shell_system_swap_for_test(NULL);

    /* a draws only; b only frees and invalidates. */
    systems[0] =
        (shell_system_t){.name = "a", .order = 10, .update = a_update, .compose = a_compose, .overlay = a_overlay};
    systems[1] = (shell_system_t){.name = "b", .order = 20, .invalidate = b_invalidate, .app_exit = b_app_exit};
    shell_system_register(&systems[1]);
    shell_system_register(&systems[0]);

    static const struct {
        const char* expected;
    } want[PHASE_COUNT] = {{"a.update"}, {"a.compose"}, {"a.overlay"}, {"b.invalidate"}, {"b.app_exit"}};

    for (int p = 0; p < PHASE_COUNT; p++) {
        call_count = 0;
        phases[p].run();
        const char* rows[1] = {want[p].expected};
        expect_calls(phases[p].phase, rows, 1);
    }

    shell_system_swap_for_test(real);
    free(systems);
    free(calls);
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

    shell_system_t* systems = malloc(sizeof(*systems) * 2);
    TEST_ASSERT_NOT_NULL(systems);
    shell_system_t* real = shell_system_swap_for_test(NULL);
    TEST_ASSERT_FALSE_MESSAGE(shell_systems_overlap_present(), "an empty registry");

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        shell_system_swap_for_test(NULL);
        systems[0] = (shell_system_t){.name = "a", .order = 10, .overlaps_present = cases[i].first};
        systems[1] = (shell_system_t){.name = "b", .order = 20, .overlaps_present = cases[i].second};
        shell_system_register(&systems[0]);
        shell_system_register(&systems[1]);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].overlaps, shell_systems_overlap_present(), cases[i].name);
    }

    shell_system_swap_for_test(real);
    free(systems);
}

void
run_shell_system_suite(void) {
    RUN_TEST(test_every_phase_runs_the_systems_by_order_whatever_order_they_registered_in);
    RUN_TEST(test_a_system_whose_phase_is_null_is_skipped_by_that_phase_alone);
    RUN_TEST(test_the_present_overlaps_when_any_system_asks_and_never_by_default);
}

#else

void
run_shell_system_suite(void) {}

#endif

SUITE_REGISTER(run_shell_system_suite);

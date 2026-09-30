/*
 * Device-only: the audit RUN_TEST wraps around every test on the board - the
 * leak check and the frame watch - holds for a test that ends in
 * TEST_PASS(), which leaves by longjmp.
 */

#include "suites.h"

#ifdef DEVICE_BUILD

#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "unity.h"

#define LEAK_BYTES       64
/* The body runs a second time when the heap dropped. */
#define RUNS_MAX         2
/* Long enough for the warm-up and then a whole window. */
#define WATCHED_PRESENTS (FRAME_WATCH_WARMUP + FRAME_WATCH_WINDOW)

static void* leaked[RUNS_MAX];
static int leaks;
static suite_test_verdict_t verdict;
static volatile bool escaped;

static void
fixture(void) {
    leaks = 0;
    verdict = (suite_test_verdict_t){0};
}

static void
free_leaks(void) {
    for (int i = 0; i < leaks; i++) {
        free(leaked[i]);
    }
    leaks = 0;
}

static void
leak_then_pass(void) {
    if (leaks < RUNS_MAX) {
        leaked[leaks++] = malloc(LEAK_BYTES);
    }
    TEST_PASS();
}

static void
allocate_every_present_then_pass(void) {
    for (int i = 0; i < WATCHED_PRESENTS; i++) {
        volatile char* block = malloc(32);
        if (block != NULL) {
            block[0] = 1;
        }
        free((void*)block);
        gfx_present();
    }
    TEST_PASS();
}

/* A TEST_PASS() the harness lets escape lands in this frame, so the test
 * sees it instead of silently passing. Afterwards the test's own abort frame
 * is put back, and its own watch, which the nested run ended, starts again. */
static void
run_trapped(void (*body)(void)) {
    jmp_buf outer;
    memcpy(outer, Unity.AbortFrame, sizeof outer);
    escaped = true;
    if (TEST_PROTECT()) {
        suite_run_body(body, &verdict);
        escaped = false;
    }
    memcpy(Unity.AbortFrame, outer, sizeof outer);
    frame_watch_test_begin();
}

static void
test_a_leak_in_a_test_that_ends_in_test_pass_is_caught(void) {
    fixture();
    run_trapped(leak_then_pass);
    free_leaks();
    TEST_ASSERT_FALSE_MESSAGE(escaped, "TEST_PASS() jumped past the leak audit");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(LEAK_BYTES, verdict.leaked_8bit);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(LEAK_BYTES, verdict.leaked_32bit);
}

static void
test_a_repeat_in_a_test_that_ends_in_test_pass_is_caught(void) {
    fixture();
    run_trapped(allocate_every_present_then_pass);
    TEST_ASSERT_FALSE_MESSAGE(escaped, "TEST_PASS() jumped past the frame watch");
    TEST_ASSERT_GREATER_THAN_INT(0, verdict.watch.repeating);
}

static void
suite_test_harness(void) {
    RUN_TEST(test_a_leak_in_a_test_that_ends_in_test_pass_is_caught);
    RUN_TEST(test_a_repeat_in_a_test_that_ends_in_test_pass_is_caught);
}

#else

static void
suite_test_harness(void) {}

#endif

SUITE_REGISTER(suite_test_harness)

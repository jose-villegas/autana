/*
 * On the board: the audit RUN_TEST wraps around every test on the board (the
 * leak check and the frame watch) holds for a test that ends in
 * TEST_PASS(), which leaves by longjmp, and does not count a task the test
 * ended as a leak. On the host: the deferred cleanups a fixture registers
 * run in reverse, once each, even when the test bailed.
 */

#include "suites.h"

#ifdef DEVICE_BUILD

#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gfx/present/gfx_present.h"
#include "test_harness.h"
#include "unity.h"

#define LEAK_BYTES       64
/* The body runs a second time when the heap dropped. */
#define RUNS_MAX         2
/* Long enough for the warm-up and then a whole window. */
#define WATCHED_PRESENTS (FRAME_WATCH_WARMUP + FRAME_WATCH_WINDOW)
#define TASK_STACK_BYTES 2048

static void* leaked[RUNS_MAX];
static int leaks;
static suite_test_verdict_t verdict;
static volatile bool escaped;
static atomic_bool spinning;
/* The task count with the spinner and without the ended task. */
static UBaseType_t spin_tasks;

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

static void
end_itself(void* main_task) {
    xTaskNotifyGive(main_task);
    vTaskDelete(NULL);
}

/* The task ends itself, so the idle task frees it later, and runs before
 * xTaskCreatePinnedToCore() returns: it outranks this one on this core. */
static void
end_a_task(void) {
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreatePinnedToCore(end_itself, "ended", TASK_STACK_BYTES, self,
                                                      uxTaskPriorityGet(NULL) + 1, NULL, xPortGetCoreID()));
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

/* Keeps the other core's idle task from running until this core's has freed
 * the ended task, so only the audit's own wait can let that happen. */
static void
spin(void* unused) {
    (void)unused;
    while (atomic_load(&spinning) && uxTaskGetNumberOfTasks() <= spin_tasks) {}
    while (atomic_load(&spinning) && uxTaskGetNumberOfTasks() > spin_tasks) {}
    vTaskDelete(NULL);
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

/* With both idle tasks held off, only the audit's own wait lets the ended
 * task be freed: without it, its stack reads as leaked every time. Once it
 * is freed, the spinner ends too, and the audit sees both idle tasks pass.
 * That last wait, for an idle task that took the task but has not yet freed
 * it, is too brief a window to hold open here. */
static void
test_a_task_the_test_ended_is_not_a_leak(void) {
    fixture();
    atomic_store(&spinning, true);
    spin_tasks = uxTaskGetNumberOfTasks() + 1;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreatePinnedToCore(spin, "spin", TASK_STACK_BYTES, NULL, uxTaskPriorityGet(NULL),
                                                      NULL, !xPortGetCoreID()));
    run_trapped(end_a_task);
    atomic_store(&spinning, false);
    TEST_ASSERT_FALSE(escaped);
    TEST_ASSERT_EQUAL_UINT32(0, verdict.leaked_8bit);
    TEST_ASSERT_EQUAL_UINT32(0, verdict.leaked_32bit);
}

static void
suite_test_harness(void) {
    RUN_TEST(test_a_leak_in_a_test_that_ends_in_test_pass_is_caught);
    RUN_TEST(test_a_repeat_in_a_test_that_ends_in_test_pass_is_caught);
    RUN_TEST(test_a_task_the_test_ended_is_not_a_leak);
}

#else

#include <setjmp.h>
#include <string.h>

#include "test_cleanup.h"
#include "unity.h"

#define RAN_MAX 4

/* The cleanups that ran, as the letters they note. */
static char ran[RAN_MAX + 1];

static void
note(char which) {
    const size_t at = strlen(ran);
    TEST_ASSERT_TRUE(at < RAN_MAX);
    ran[at] = which;
    ran[at + 1] = '\0';
}

static void
cleanup_a(void) {
    note('a');
}

static void
cleanup_b(void) {
    note('b');
}

/* Registers cleanup_a, then bails the way a failed assertion does, without
 * the failure. */
static void
register_then_bail(void) {
    suite_add_test_cleanup(cleanup_a);
    TEST_ABORT();
}

/* Runs `body` in a frame of its own, so its bail ends it alone, and puts the
 * test's abort frame back. */
static void
run_trapped(void (*body)(void)) {
    jmp_buf outer;
    memcpy(outer, Unity.AbortFrame, sizeof outer);
    if (TEST_PROTECT()) {
        body();
    }
    memcpy(Unity.AbortFrame, outer, sizeof outer);
}

static void
test_cleanups_run_in_reverse_registration_order(void) {
    ran[0] = '\0';
    suite_add_test_cleanup(cleanup_a);
    suite_add_test_cleanup(cleanup_b);
    suite_run_test_cleanup();
    TEST_ASSERT_EQUAL_STRING("ba", ran);
}

static void
test_a_cleanup_registered_twice_runs_once(void) {
    ran[0] = '\0';
    suite_add_test_cleanup(cleanup_a);
    suite_add_test_cleanup(cleanup_a);
    suite_run_test_cleanup();
    suite_run_test_cleanup();
    TEST_ASSERT_EQUAL_STRING("a", ran);
}

static void
test_a_cleanup_registered_by_a_test_that_bails_still_runs(void) {
    ran[0] = '\0';
    run_trapped(register_then_bail);
    TEST_ASSERT_EQUAL_STRING("", ran);
    suite_run_test_cleanup();
    TEST_ASSERT_EQUAL_STRING("a", ran);
}

static void
suite_test_harness(void) {
    RUN_TEST(test_cleanups_run_in_reverse_registration_order);
    RUN_TEST(test_a_cleanup_registered_twice_runs_once);
    RUN_TEST(test_a_cleanup_registered_by_a_test_that_bails_still_runs);
}

#endif

SUITE_REGISTER(suite_test_harness)

/* Portable suite: core-1 jobs execute inline on a host. */
#include <stdbool.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "util/job.h"

#ifdef DEVICE_BUILD
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

typedef struct {
    int value;
    int* observed;
} job_test_ctx_t;

static void
job_test_increment(void* ctx) {
    job_test_ctx_t* const work = ctx;
    work->value++;
    *work->observed = work->value;
}

static int job_test_calls;

static void
job_test_count(void* ctx) {
    (void)ctx;
    job_test_calls++;
}

static void
test_a_host_job_runs_inline_and_waits(void) {
    int observed = 0;
    job_test_ctx_t ctx = {.value = 41, .observed = &observed};

    TEST_ASSERT_TRUE(job_run_core1(job_test_increment, &ctx, sizeof ctx));
    TEST_ASSERT_EQUAL_INT(42, observed);
    TEST_ASSERT_TRUE(job_wait(0));
}

static void
test_a_job_callback_cannot_change_the_callers_context(void) {
    int observed = 0;
    job_test_ctx_t ctx = {.value = 7, .observed = &observed};

    TEST_ASSERT_TRUE(job_run_core1(job_test_increment, &ctx, sizeof ctx));
    TEST_ASSERT_EQUAL_INT(8, observed);
    TEST_ASSERT_EQUAL_INT(7, ctx.value);
}

static void
test_consecutive_host_jobs_run_inline(void) {
    int first = 0;
    int second = 0;

    job_test_calls = 0;
    TEST_ASSERT_TRUE(job_run_core1(job_test_count, &first, sizeof first));
    TEST_ASSERT_TRUE(job_wait(0));
    TEST_ASSERT_TRUE(job_run_core1(job_test_count, &second, sizeof second));
    TEST_ASSERT_TRUE(job_wait(0));
    TEST_ASSERT_EQUAL_INT(2, job_test_calls);
}

static void
test_the_context_limit_accepts_its_boundary_and_rejects_the_next_byte(void) {
    uint8_t largest[JOB_CTX_MAX] = {0};
    uint8_t too_large[JOB_CTX_MAX + 1] = {0};
    job_test_calls = 0;

    TEST_ASSERT_TRUE(job_run_core1(job_test_count, largest, sizeof largest));
    TEST_ASSERT_TRUE(job_wait(0));
    TEST_ASSERT_EQUAL_INT(1, job_test_calls);
    TEST_ASSERT_FALSE(job_run_core1(job_test_count, too_large, sizeof too_large));
    TEST_ASSERT_EQUAL_INT(1, job_test_calls);
}

#ifndef DEVICE_BUILD
/* The contract a caller whose job blocks on the caller's own progress relies
 * on: no core 1, no run at all. Inline is what it must never be. */
static void
test_a_host_try_refuses_rather_than_running_inline(void) {
    job_test_calls = 0;

    TEST_ASSERT_FALSE(job_try_core1(job_test_count, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, job_test_calls);
    TEST_ASSERT_TRUE(job_wait(0));
}
#endif

#ifdef DEVICE_BUILD
typedef struct {
    volatile bool* finished;
} job_slow_ctx_t;

static void
job_test_slow(void* ctx) {
    const job_slow_ctx_t* const work = ctx;
    vTaskDelay(pdMS_TO_TICKS(20));
    *work->finished = true;
}

static void
test_a_try_on_an_idle_worker_reaches_core_1(void) {
    volatile bool finished = false;
    const job_slow_ctx_t slow = {.finished = &finished};

    TEST_ASSERT_TRUE(job_try_core1(job_test_slow, &slow, sizeof slow));
    TEST_ASSERT_TRUE(job_wait(100));
    TEST_ASSERT_TRUE(finished);
}

static void
test_a_try_refuses_while_a_job_is_outstanding(void) {
    volatile bool finished = false;
    const job_slow_ctx_t slow = {.finished = &finished};

    job_test_calls = 0;
    TEST_ASSERT_TRUE(job_try_core1(job_test_slow, &slow, sizeof slow));
    TEST_ASSERT_FALSE(job_try_core1(job_test_count, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, job_test_calls);
    TEST_ASSERT_TRUE(job_wait(100));
    TEST_ASSERT_TRUE(finished);
}
#endif

void
suite_job(void) {
    RUN_TEST(test_a_host_job_runs_inline_and_waits);
    RUN_TEST(test_a_job_callback_cannot_change_the_callers_context);
    RUN_TEST(test_consecutive_host_jobs_run_inline);
    RUN_TEST(test_the_context_limit_accepts_its_boundary_and_rejects_the_next_byte);
#ifndef DEVICE_BUILD
    RUN_TEST(test_a_host_try_refuses_rather_than_running_inline);
#endif
#ifdef DEVICE_BUILD
    RUN_TEST(test_a_try_on_an_idle_worker_reaches_core_1);
    RUN_TEST(test_a_try_refuses_while_a_job_is_outstanding);
#endif
}

SUITE_REGISTER(suite_job)

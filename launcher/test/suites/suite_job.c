/* Portable suite: core-1 jobs execute inline on a host. */
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "util/job.h"

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

/* A host job has run by the time job_run_core1() returns. On the device it
 * is on the other core, and reading its result before collecting it races. */
#ifdef DEVICE_BUILD
#define JOB_TEST_WAIT_MS 100
#else
#define JOB_TEST_WAIT_MS 0
#endif

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
#ifndef DEVICE_BUILD
    TEST_ASSERT_EQUAL_INT_MESSAGE(42, observed, "a host job must have run inline");
#endif
    TEST_ASSERT_TRUE(job_wait(JOB_TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_INT(42, observed);
}

static void
test_a_job_callback_cannot_change_the_callers_context(void) {
    int observed = 0;
    job_test_ctx_t ctx = {.value = 7, .observed = &observed};

    TEST_ASSERT_TRUE(job_run_core1(job_test_increment, &ctx, sizeof ctx));
    TEST_ASSERT_TRUE(job_wait(JOB_TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_INT(8, observed);
    TEST_ASSERT_EQUAL_INT(7, ctx.value);
}

static void
test_consecutive_host_jobs_run_inline(void) {
    int first = 0;
    int second = 0;

    job_test_calls = 0;
    TEST_ASSERT_TRUE(job_run_core1(job_test_count, &first, sizeof first));
    TEST_ASSERT_TRUE(job_wait(JOB_TEST_WAIT_MS));
    TEST_ASSERT_TRUE(job_run_core1(job_test_count, &second, sizeof second));
    TEST_ASSERT_TRUE(job_wait(JOB_TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_INT(2, job_test_calls);
}

static void
test_the_context_limit_accepts_its_boundary_and_rejects_the_next_byte(void) {
    uint8_t largest[JOB_CTX_MAX] = {0};
    uint8_t too_large[JOB_CTX_MAX + 1] = {0};
    job_test_calls = 0;

    TEST_ASSERT_TRUE(job_run_core1(job_test_count, largest, sizeof largest));
    TEST_ASSERT_TRUE(job_wait(JOB_TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_INT(1, job_test_calls);
    TEST_ASSERT_FALSE(job_run_core1(job_test_count, too_large, sizeof too_large));
    TEST_ASSERT_EQUAL_INT(1, job_test_calls);
}

void
suite_job(void) {
    RUN_TEST(test_a_host_job_runs_inline_and_waits);
    RUN_TEST(test_a_job_callback_cannot_change_the_callers_context);
    RUN_TEST(test_consecutive_host_jobs_run_inline);
    RUN_TEST(test_the_context_limit_accepts_its_boundary_and_rejects_the_next_byte);
}

SUITE_REGISTER(suite_job)

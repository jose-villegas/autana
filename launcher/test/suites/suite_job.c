/* Portable suite: core-1 jobs execute inline on a host. */
#include <stdbool.h>
#include <stdint.h>

#include "suites.h"
#include "unity.h"

#include "core/job.h"
#include "core/timing.h"

#ifdef DEVICE_BUILD
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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

static void
test_share_update_converges_and_clamps(void) {
    const int damped = job_share_update(JOB_SHARE_EVEN, JOB_SHARE_EVEN, 3 * JOB_SHARE_EVEN);
    const int balanced_high = JOB_SHARE_PERMILLE * 3 / 4;
    const int balanced_low = JOB_SHARE_PERMILLE / 4;
    TEST_ASSERT_INT_WITHIN(1, (JOB_SHARE_EVEN + balanced_high) / 2, damped);
    TEST_ASSERT_INT_WITHIN(1, (JOB_SHARE_EVEN + balanced_low) / 2,
                           job_share_update(JOB_SHARE_EVEN, 3 * JOB_SHARE_EVEN, JOB_SHARE_EVEN));
    TEST_ASSERT_EQUAL_INT(job_share_update(JOB_SHARE_EVEN, 1, 2),
                          job_share_update(JOB_SHARE_EVEN, INT64_C(3000000000), INT64_C(6000000000)));
    int above = JOB_SHARE_EVEN;
    for (int i = 0; i < 12; i++) {
        const int next = job_share_update(above, 3 * above, JOB_SHARE_PERMILLE - above);
        TEST_ASSERT_TRUE(next <= above && next >= JOB_SHARE_PERMILLE / 4);
        above = next;
    }
    TEST_ASSERT_INT_WITHIN(2, JOB_SHARE_PERMILLE / 4, above);
    int share = JOB_SHARE_EVEN;
    const int balanced = JOB_SHARE_PERMILLE * 3 / 4;
    for (int i = 0; i < 12; i++) {
        const int next = job_share_update(share, share, 3 * (JOB_SHARE_PERMILLE - share));
        TEST_ASSERT_TRUE(next >= share && next <= balanced);
        share = next;
    }
    TEST_ASSERT_INT_WITHIN(2, balanced, share);
    TEST_ASSERT_EQUAL_INT(JOB_SHARE_MIN, job_share_update(JOB_SHARE_MIN, 1000000, 1));
    TEST_ASSERT_EQUAL_INT(JOB_SHARE_MAX, job_share_update(JOB_SHARE_MAX, 1, 1000000));
    TEST_ASSERT_EQUAL_INT(JOB_SHARE_EVEN, job_share_update(JOB_SHARE_EVEN, 1, 1));
    TEST_ASSERT_EQUAL_INT(share, job_share_update(share, 0, 1));
    TEST_ASSERT_EQUAL_INT(share, job_share_update(share, 1, -1));
    TEST_ASSERT_EQUAL_INT(share, job_share_update(share, 1, 0));
    TEST_ASSERT_EQUAL_INT(share, job_share_update(share, -1, 1));
    TEST_ASSERT_EQUAL_INT(share, job_share_update(share, 0, -1));
}

static void
test_share_counts_round_down_within_the_total(void) {
    TEST_ASSERT_EQUAL_INT(3, job_share_core1_count(0, 7));
    TEST_ASSERT_EQUAL_INT(0, job_share_core1_count(0, 1));
    TEST_ASSERT_EQUAL_INT(15, job_share_core1_count(JOB_SHARE_MIN - JOB_SHARE_EVEN, 100));
    TEST_ASSERT_EQUAL_INT(85, job_share_core1_count(JOB_SHARE_MAX - JOB_SHARE_EVEN, 100));
}

/* Spins rather than sleeps, so its time is the work a core does. */
typedef struct {
    int64_t us;
} job_spin_ctx_t;

#define JOB_SPIN_SHORT_US 1000
#define JOB_SPIN_LONG_US  (4 * JOB_SPIN_SHORT_US)

static void
job_test_spin(void* ctx) {
    const job_spin_ctx_t* const spin = ctx;
    const int64_t until = timing_now_us() + spin->us;
    while (timing_now_us() < until) {}
}

/* Inline on a host, so the slower second half stands in for a core 1 that
 * is busy elsewhere: the steered share moves away from it. */
static void
test_a_steered_split_gives_the_slower_half_less(void) {
    int16_t core1_offset = 0;
    job_spin_ctx_t first = {.us = JOB_SPIN_SHORT_US};
    const job_spin_ctx_t second = {.us = JOB_SPIN_LONG_US};
    (void)job_run_steered(&core1_offset, job_test_spin, &first, &second, sizeof first);
    TEST_ASSERT_LESS_THAN_INT(0, core1_offset);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(JOB_SHARE_MIN, JOB_SHARE_EVEN + core1_offset);
}

#ifndef DEVICE_BUILD
static void
test_a_host_split_reports_inline_and_finishes_both_ranges(void) {
    int first = 0;
    int second = 0;
    job_test_ctx_t a = {.value = 0, .observed = &first};
    job_test_ctx_t b = {.value = 0, .observed = &second};
    TEST_ASSERT_FALSE(job_run_split(job_test_increment, &a, &b, sizeof a));
    TEST_ASSERT_EQUAL_INT(1, first);
    TEST_ASSERT_EQUAL_INT(1, second);
}

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
    timing_sleep_ms(20);
    *work->finished = true;
}

static void
test_an_idle_worker_split_runs_both_halves_on_device(void) {
    TEST_ASSERT_TRUE(job_wait(JOB_TEST_WAIT_MS));
    int first = 0;
    int second = 0;
    job_test_ctx_t a = {.value = 0, .observed = &first};
    const job_test_ctx_t b = {.value = 0, .observed = &second};
    TEST_ASSERT_TRUE(job_run_split(job_test_increment, &a, &b, sizeof a));
    TEST_ASSERT_EQUAL_INT(1, first);
    TEST_ASSERT_EQUAL_INT(1, second);
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

#define JOB_HOG_US       (20 * JOB_SPIN_SHORT_US)
/* Above the worker's priority, as the panel present task is. */
#define JOB_HOG_PRIORITY (configMAX_PRIORITIES - 2)

typedef struct {
    SemaphoreHandle_t started, done;
} job_hog_t;

static void
job_test_hog(void* arg) {
    job_hog_t* const hog = arg;
    xSemaphoreGive(hog->started);
    job_spin_ctx_t spin = {.us = JOB_HOG_US};
    job_test_spin(&spin);
    xSemaphoreGive(hog->done);
    vTaskDelete(NULL);
}

/* A higher-priority task holding core 1, as present does, delays the
 * worker's half. Core 1's time counts from dispatch, so the steered share
 * moves off that core even though both halves hold equal work. */
static void
test_a_task_holding_core_1_steers_work_off_it(void) {
    job_hog_t hog = {xSemaphoreCreateBinary(), xSemaphoreCreateBinary()};
    TEST_ASSERT_NOT_NULL(hog.started);
    TEST_ASSERT_NOT_NULL(hog.done);
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreatePinnedToCore(job_test_hog, "job_hog", 2048, &hog, JOB_HOG_PRIORITY, NULL, 1));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(hog.started, pdMS_TO_TICKS(JOB_TEST_WAIT_MS)));
    int16_t core1_offset = 0;
    job_spin_ctx_t first = {.us = JOB_SPIN_SHORT_US};
    const job_spin_ctx_t second = {.us = JOB_SPIN_SHORT_US};
    TEST_ASSERT_TRUE(job_run_steered(&core1_offset, job_test_spin, &first, &second, sizeof first));
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(JOB_HOG_US / 2, job_core1_elapsed_us());
    TEST_ASSERT_LESS_THAN_INT(0, core1_offset);
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(hog.done, pdMS_TO_TICKS(JOB_TEST_WAIT_MS)));
    vSemaphoreDelete(hog.started);
    vSemaphoreDelete(hog.done);
}
#endif

void
suite_job(void) {
    RUN_TEST(test_a_host_job_runs_inline_and_waits);
    RUN_TEST(test_a_job_callback_cannot_change_the_callers_context);
    RUN_TEST(test_consecutive_host_jobs_run_inline);
    RUN_TEST(test_the_context_limit_accepts_its_boundary_and_rejects_the_next_byte);
    RUN_TEST(test_share_update_converges_and_clamps);
    RUN_TEST(test_share_counts_round_down_within_the_total);
    RUN_TEST(test_a_steered_split_gives_the_slower_half_less);
#ifndef DEVICE_BUILD
    RUN_TEST(test_a_host_try_refuses_rather_than_running_inline);
    RUN_TEST(test_a_host_split_reports_inline_and_finishes_both_ranges);
#endif
#ifdef DEVICE_BUILD
    RUN_TEST(test_an_idle_worker_split_runs_both_halves_on_device);
    RUN_TEST(test_a_try_on_an_idle_worker_reaches_core_1);
    RUN_TEST(test_a_try_refuses_while_a_job_is_outstanding);
    RUN_TEST(test_a_task_holding_core_1_steers_work_off_it);
#endif
}

SUITE_REGISTER(suite_job)

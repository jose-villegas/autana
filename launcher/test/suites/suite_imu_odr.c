/* imu_odr: a requested sensor rate switches, or falls back to the rate that ran. */
#include "suites.h"
#include "unity.h"

#include <stdlib.h>
#include "input/motion/imu.h"
#include "input/motion/imu_odr.h"
#include "input/motion/imu_queue.h"

#define TEST_CALLS_MAX 4

typedef struct {
    unsigned calls[TEST_CALLS_MAX];
    int count;
    int fail_first; /* this many configure() calls fail before any succeeds */
} test_bus_t;

static test_bus_t bus;
static bool fifo_gap;
static imu_demand_level_t level;

static bool
fake_configure(unsigned odr_code) {
    TEST_ASSERT_LESS_THAN_INT(TEST_CALLS_MAX, bus.count);
    bus.calls[bus.count++] = odr_code;
    return bus.count > bus.fail_first;
}

static void
reset(int fail_first) {
    bus = (test_bus_t){.fail_first = fail_first};
    fifo_gap = false;
    level = IMU_DEMAND_NONE;
}

static void
test_nothing_asked_touches_nothing(void) {
    reset(0);
    fifo_gap = true;
    imu_odr_t odr = {.requested = IMU_ODR_CODE, .applied = IMU_ODR_CODE};
    TEST_ASSERT_EQUAL_INT(IMU_ODR_UNCHANGED, imu_odr_apply(&odr, fake_configure, &fifo_gap, &level));
    TEST_ASSERT_EQUAL_INT(0, bus.count);
    TEST_ASSERT_TRUE(fifo_gap);
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_NONE, level);
}

static void
test_switch_applies_the_request(void) {
    reset(0);
    imu_odr_t odr = {.requested = IMU_BUZZ_ODR_CODE, .applied = IMU_ODR_CODE};
    TEST_ASSERT_EQUAL_INT(IMU_ODR_SWITCHED, imu_odr_apply(&odr, fake_configure, &fifo_gap, &level));
    TEST_ASSERT_EQUAL_UINT(IMU_BUZZ_ODR_CODE, odr.applied);
    TEST_ASSERT_EQUAL_INT(1, bus.count);
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_FIFO, level);
    TEST_ASSERT_EQUAL_INT(IMU_ODR_UNCHANGED, imu_odr_apply(&odr, fake_configure, &fifo_gap, &level));
}

/* A partial write can leave the sensor off: the old rate is configured again. */
static void
test_failed_switch_restores_the_old_rate(void) {
    reset(1);
    imu_odr_t odr = {.requested = IMU_BUZZ_ODR_CODE, .applied = IMU_ODR_CODE};
    TEST_ASSERT_EQUAL_INT(IMU_ODR_RESTORED, imu_odr_apply(&odr, fake_configure, &fifo_gap, &level));
    TEST_ASSERT_EQUAL_INT(2, bus.count);
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_FIFO, level);
    TEST_ASSERT_EQUAL_UINT(IMU_BUZZ_ODR_CODE, bus.calls[0]);
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, bus.calls[1]);
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, odr.applied);
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, odr.requested);
}

static void
test_failed_restore_is_lost(void) {
    reset(2);
    imu_odr_t odr = {.requested = IMU_BUZZ_ODR_CODE, .applied = IMU_ODR_CODE};
    TEST_ASSERT_EQUAL_INT(IMU_ODR_LOST, imu_odr_apply(&odr, fake_configure, &fifo_gap, &level));
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_NONE, level);
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, odr.applied);
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, odr.requested);
}

static void
test_switch_gap_is_taken_before_next_drain(void) {
    for (int failures = 0; failures <= 2; failures++) {
        reset(failures);
        imu_odr_t odr = {.requested = IMU_BUZZ_ODR_CODE, .applied = IMU_ODR_CODE};
        imu_queue_t* queue = calloc(1, sizeof *queue);
        TEST_ASSERT_NOT_NULL(queue);
        fifo_gap = true;
        TEST_ASSERT_NOT_EQUAL(IMU_ODR_UNCHANGED, imu_odr_apply(&odr, fake_configure, &fifo_gap, &level));
        imu_queue_set_rate(queue, odr.applied);
        imu_sample_t sample = {.ax = IMU_COUNTS_PER_G};
        uint8_t lost = 0;
        bool gap = false;
        TEST_ASSERT_EQUAL_INT(0, imu_queue_take_at(queue, odr.applied, &sample, &lost, &gap));
        TEST_ASSERT_TRUE(gap);
        imu_queue_push_drain(queue, &sample, 1, 0, fifo_gap);
        TEST_ASSERT_EQUAL_INT(1, imu_queue_take_at(queue, odr.applied, &sample, &lost, &gap));
        free(queue);
        TEST_ASSERT_FALSE(gap);
    }
}

static void
test_buzz_period_fits_retained_history(void) {
    const unsigned slowest_ms = IMU_FIFO_SAMPLES * IMU_SAMPLE_US / MATH_US_PER_MS;
    for (unsigned requested_ms = 1; requested_ms <= slowest_ms; requested_ms++) {
        const unsigned buzz_ms = imu_odr_period_ms(IMU_BUZZ_ODR_CODE, requested_ms);
        TEST_ASSERT_GREATER_THAN_UINT(0, buzz_ms);
        TEST_ASSERT_LESS_OR_EQUAL_UINT(requested_ms, buzz_ms);
        TEST_ASSERT_LESS_OR_EQUAL_UINT(IMU_BUZZ_DRAIN_MAX_MS, buzz_ms);
        TEST_ASSERT_EQUAL_UINT(requested_ms, imu_odr_period_ms(IMU_ODR_CODE, requested_ms));
    }
}

static void
test_rate_ack_waits_for_first_batch(void) {
    imu_odr_t odr = {.requested = IMU_BUZZ_ODR_CODE, .applied = IMU_BUZZ_ODR_CODE, .sequence = 1, .served_sequence = 1};
    for (unsigned empty_drains = 0; empty_drains < IMU_FIFO_SAMPLES; empty_drains++) {
        TEST_ASSERT_FALSE(imu_odr_ready(&odr, odr.sequence));
    }
    odr.flowing = true;
    TEST_ASSERT_TRUE(imu_odr_ready(&odr, odr.sequence));
    TEST_ASSERT_FALSE(imu_odr_ready(&odr, odr.sequence + 1));
}

void
run_imu_odr_suite(void) {
    RUN_TEST(test_rate_ack_waits_for_first_batch);
    RUN_TEST(test_buzz_period_fits_retained_history);
    RUN_TEST(test_switch_gap_is_taken_before_next_drain);
    RUN_TEST(test_nothing_asked_touches_nothing);
    RUN_TEST(test_switch_applies_the_request);
    RUN_TEST(test_failed_switch_restores_the_old_rate);
    RUN_TEST(test_failed_restore_is_lost);
}

SUITE_REGISTER(run_imu_odr_suite);

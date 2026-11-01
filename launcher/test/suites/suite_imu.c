/* imu: driver sequencing without a bus.
 * imu_odr: a requested sensor rate switches, or falls back to the rate that ran.
 * imu_reset: every boot soft-resets the sensor and waits until it is done; imu_poll waits on masked bits. */
#include <stdlib.h>

#include "suites.h"
#include "testkit/cleanup.h"
#include "unity.h"

#include "input/motion/imu.h"
#include "input/motion/imu_odr.h"
#include "input/motion/imu_queue.h"
#include "input/motion/imu_reset.h"

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

/* The fake sensor for imu_reset: reads `busy_value` until `done_at_us`, then the done value. */
#define TEST_RESET_CALLS_MAX 64
#define STATUSINT_TEST_BIT   0x80
#define TEST_STALE_VALUE     0x81 /* the done bit set among others: not the done value */

typedef enum { CALL_RESET, CALL_POLL } reset_call_t;

typedef struct {
    reset_call_t calls[TEST_RESET_CALLS_MAX];
    int count;
    int nack_polls;
    bool reset_nack;
    uint8_t busy_value;
    int64_t done_at_us;
    int64_t now_us;
    int64_t first_poll_us;
} test_sensor_t;

static test_sensor_t* sensor;

static void
free_sensor(void) {
    free(sensor);
    sensor = NULL;
}

static void
new_sensor(int64_t done_at_us) {
    suite_set_test_cleanup(free_sensor);
    sensor = calloc(1, sizeof *sensor);
    TEST_ASSERT_NOT_NULL(sensor);
    sensor->done_at_us = done_at_us;
    sensor->first_poll_us = -1;
}

static void
record(reset_call_t call) {
    TEST_ASSERT_LESS_THAN_INT(TEST_RESET_CALLS_MAX, sensor->count);
    sensor->calls[sensor->count++] = call;
}

static int
polls(void) {
    int n = 0;
    for (int i = 0; i < sensor->count; i++) {
        n += sensor->calls[i] == CALL_POLL;
    }
    return n;
}

static bool
sensor_write(uint8_t reg, uint8_t value) {
    TEST_ASSERT_EQUAL_HEX8(REG_RESET, reg);
    TEST_ASSERT_EQUAL_HEX8(RESET_COMMAND, value);
    record(CALL_RESET);
    return !sensor->reset_nack;
}

static bool
sensor_read(uint8_t reg, uint8_t* out, size_t len) {
    TEST_ASSERT_EQUAL_HEX8(REG_RESET_RESULT, reg);
    TEST_ASSERT_EQUAL_UINT(1, len);
    if (sensor->first_poll_us < 0) {
        sensor->first_poll_us = sensor->now_us;
    }
    const bool nack = polls() < sensor->nack_polls;
    record(CALL_POLL);
    if (nack) {
        return false;
    }
    *out = sensor->now_us >= sensor->done_at_us ? RESET_DONE : sensor->busy_value;
    return true;
}

static int64_t
sensor_now(void) {
    return sensor->now_us;
}

static void
sensor_delay(uint32_t us) {
    sensor->now_us += us;
}

static bool
reset_sensor(void) {
    return imu_reset(sensor_write, sensor_read, sensor_now, sensor_delay);
}

/* The last poll inside the window lands one poll period before the deadline. */
static int64_t
last_poll_us(void) {
    return IMU_RESET_SETTLE_US + IMU_RESET_TIMEOUT_US - IMU_RESET_POLL_US;
}

static void
test_reset_settles_then_polls_until_done(void) {
    new_sensor(IMU_RESET_SETTLE_US + (2 * IMU_RESET_POLL_US));
    TEST_ASSERT_TRUE(reset_sensor());
    TEST_ASSERT_EQUAL_INT(CALL_RESET, sensor->calls[0]);
    TEST_ASSERT_EQUAL_INT(3, polls());
    TEST_ASSERT_EQUAL_INT64(IMU_RESET_SETTLE_US, sensor->first_poll_us);
}

static void
test_reset_tolerates_nacks_while_resetting(void) {
    new_sensor(0);
    sensor->reset_nack = true;
    sensor->nack_polls = 2;
    TEST_ASSERT_TRUE(reset_sensor());
    TEST_ASSERT_EQUAL_INT(3, polls());
    TEST_ASSERT_EQUAL_INT64(IMU_RESET_SETTLE_US, sensor->first_poll_us);
}

static void
test_reset_waits_through_values_that_are_not_done(void) {
    new_sensor(IMU_RESET_SETTLE_US + IMU_RESET_POLL_US);
    sensor->busy_value = TEST_STALE_VALUE;
    TEST_ASSERT_TRUE(reset_sensor());
    TEST_ASSERT_EQUAL_INT(2, polls());
}

static void
test_reset_never_done_fails(void) {
    new_sensor(INT64_MAX);
    sensor->busy_value = TEST_STALE_VALUE;
    TEST_ASSERT_FALSE(reset_sensor());
}

static void
test_reset_done_on_the_last_poll_counts(void) {
    new_sensor(last_poll_us());
    TEST_ASSERT_TRUE(reset_sensor());
}

static void
test_reset_done_after_the_deadline_fails(void) {
    new_sensor(last_poll_us() + IMU_RESET_POLL_US);
    TEST_ASSERT_FALSE(reset_sensor());
    TEST_ASSERT_EQUAL_INT64(IMU_RESET_SETTLE_US + IMU_RESET_TIMEOUT_US, sensor->now_us);
}

static uint8_t poll_value;

static bool
read_poll_value(uint8_t reg, uint8_t* out, size_t len) {
    *out = poll_value;
    return true;
}

static void
no_delay(uint32_t us) {}

static int64_t
fixed_now(void) {
    return 0;
}

/* wait_command(false) waits for a clear bit: the mask keeps the other bits out. */
static void
test_poll_compares_only_the_masked_bits(void) {
    poll_value = (uint8_t)~STATUSINT_TEST_BIT;
    TEST_ASSERT_TRUE(imu_poll(read_poll_value, 0, STATUSINT_TEST_BIT, 0, fixed_now, no_delay, 0, 0));
    poll_value = STATUSINT_TEST_BIT;
    TEST_ASSERT_FALSE(imu_poll(read_poll_value, 0, STATUSINT_TEST_BIT, 0, fixed_now, no_delay, 0, 0));
}

void
run_imu_suite(void) {
    RUN_TEST(test_nothing_asked_touches_nothing);
    RUN_TEST(test_switch_applies_the_request);
    RUN_TEST(test_failed_switch_restores_the_old_rate);
    RUN_TEST(test_failed_restore_is_lost);
    RUN_TEST(test_rate_ack_waits_for_first_batch);
    RUN_TEST(test_buzz_period_fits_retained_history);
    RUN_TEST(test_switch_gap_is_taken_before_next_drain);
    RUN_TEST(test_reset_settles_then_polls_until_done);
    RUN_TEST(test_reset_tolerates_nacks_while_resetting);
    RUN_TEST(test_reset_waits_through_values_that_are_not_done);
    RUN_TEST(test_reset_never_done_fails);
    RUN_TEST(test_reset_done_on_the_last_poll_counts);
    RUN_TEST(test_reset_done_after_the_deadline_fails);
    RUN_TEST(test_poll_compares_only_the_masked_bits);
}

SUITE_REGISTER(run_imu_suite);

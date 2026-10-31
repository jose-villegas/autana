/* imu_odr: a requested sensor rate switches, or falls back to the rate that ran.
 * imu_bringup: every boot soft-resets the sensor, waits for it, then configures. */
#include <stdlib.h>

#include "suites.h"
#include "testkit/cleanup.h"
#include "unity.h"

#include "input/motion/imu.h"
#include "input/motion/imu_bringup.h"
#include "input/motion/imu_odr.h"

#define TEST_CALLS_MAX 4

typedef struct {
    unsigned calls[TEST_CALLS_MAX];
    int count;
    int fail_first; /* this many configure() calls fail before any succeeds */
} test_bus_t;

static test_bus_t bus;

static bool
fake_configure(unsigned odr_code) {
    TEST_ASSERT_LESS_THAN_INT(TEST_CALLS_MAX, bus.count);
    bus.calls[bus.count++] = odr_code;
    return bus.count > bus.fail_first;
}

static void
reset(int fail_first) {
    bus = (test_bus_t){.fail_first = fail_first};
}

static void
test_nothing_asked_touches_nothing(void) {
    reset(0);
    imu_odr_t odr = {.requested = IMU_ODR_CODE, .applied = IMU_ODR_CODE};
    TEST_ASSERT_EQUAL_INT(IMU_ODR_UNCHANGED, imu_odr_apply(&odr, fake_configure));
    TEST_ASSERT_EQUAL_INT(0, bus.count);
}

static void
test_switch_applies_the_request(void) {
    reset(0);
    imu_odr_t odr = {.requested = IMU_BUZZ_ODR_CODE, .applied = IMU_ODR_CODE};
    TEST_ASSERT_EQUAL_INT(IMU_ODR_SWITCHED, imu_odr_apply(&odr, fake_configure));
    TEST_ASSERT_EQUAL_UINT(IMU_BUZZ_ODR_CODE, odr.applied);
    TEST_ASSERT_EQUAL_INT(1, bus.count);
    TEST_ASSERT_EQUAL_INT(IMU_ODR_UNCHANGED, imu_odr_apply(&odr, fake_configure));
}

/* A partial write can leave the sensor off: the old rate is configured again. */
static void
test_failed_switch_restores_the_old_rate(void) {
    reset(1);
    imu_odr_t odr = {.requested = IMU_BUZZ_ODR_CODE, .applied = IMU_ODR_CODE};
    TEST_ASSERT_EQUAL_INT(IMU_ODR_RESTORED, imu_odr_apply(&odr, fake_configure));
    TEST_ASSERT_EQUAL_INT(2, bus.count);
    TEST_ASSERT_EQUAL_UINT(IMU_BUZZ_ODR_CODE, bus.calls[0]);
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, bus.calls[1]);
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, odr.applied);
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, odr.requested);
}

static void
test_failed_restore_is_lost(void) {
    reset(2);
    imu_odr_t odr = {.requested = IMU_BUZZ_ODR_CODE, .applied = IMU_ODR_CODE};
    TEST_ASSERT_EQUAL_INT(IMU_ODR_LOST, imu_odr_apply(&odr, fake_configure));
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, odr.applied);
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, odr.requested);
}

/* The fake sensor for imu_bringup: answers the done value after `busy_polls` polls. */
#define TEST_BRINGUP_CALLS_MAX 64

typedef enum { CALL_RESET, CALL_POLL, CALL_CONFIGURE } bringup_call_t;

typedef struct {
    bringup_call_t calls[TEST_BRINGUP_CALLS_MAX];
    int count;
    int busy_polls;
    int nack_polls;
    bool reset_nack;
    bool configure_fails;
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
new_sensor(int busy_polls) {
    suite_set_test_cleanup(free_sensor);
    sensor = calloc(1, sizeof *sensor);
    TEST_ASSERT_NOT_NULL(sensor);
    sensor->busy_polls = busy_polls;
    sensor->first_poll_us = -1;
}

static void
record(bringup_call_t call) {
    TEST_ASSERT_LESS_THAN_INT(TEST_BRINGUP_CALLS_MAX, sensor->count);
    sensor->calls[sensor->count++] = call;
}

static int
calls_of(bringup_call_t call) {
    int n = 0;
    for (int i = 0; i < sensor->count; i++) {
        n += sensor->calls[i] == call;
    }
    return n;
}

static bool
sensor_write(uint8_t reg, uint8_t value) {
    TEST_ASSERT_EQUAL_HEX8(QMI8658_REG_RESET, reg);
    TEST_ASSERT_EQUAL_HEX8(QMI8658_RESET_COMMAND, value);
    record(CALL_RESET);
    return !sensor->reset_nack;
}

static bool
sensor_read(uint8_t reg, uint8_t* out, size_t len) {
    TEST_ASSERT_EQUAL_HEX8(QMI8658_REG_RESET_RESULT, reg);
    TEST_ASSERT_EQUAL_UINT(1, len);
    if (sensor->first_poll_us < 0) {
        sensor->first_poll_us = sensor->now_us;
    }
    const int polls = calls_of(CALL_POLL);
    record(CALL_POLL);
    if (polls < sensor->nack_polls) {
        return false;
    }
    *out = polls < sensor->busy_polls ? 0 : QMI8658_RESET_DONE;
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
sensor_configure(unsigned odr_code) {
    TEST_ASSERT_EQUAL_UINT(IMU_ODR_CODE, odr_code);
    record(CALL_CONFIGURE);
    return !sensor->configure_fails;
}

static bool
bring_up(void) {
    return imu_bringup(IMU_ODR_CODE, sensor_write, sensor_read, sensor_now, sensor_delay, sensor_configure);
}

static void
test_bringup_resets_waits_then_configures(void) {
    new_sensor(2);
    TEST_ASSERT_TRUE(bring_up());
    TEST_ASSERT_EQUAL_INT(CALL_RESET, sensor->calls[0]);
    TEST_ASSERT_EQUAL_INT(3, calls_of(CALL_POLL));
    TEST_ASSERT_EQUAL_INT(1, calls_of(CALL_CONFIGURE));
    TEST_ASSERT_EQUAL_INT(CALL_CONFIGURE, sensor->calls[sensor->count - 1]);
    TEST_ASSERT_EQUAL_INT64(IMU_RESET_SETTLE_US, sensor->first_poll_us);
}

static void
test_bringup_tolerates_nacks_while_resetting(void) {
    new_sensor(0);
    sensor->reset_nack = true;
    sensor->nack_polls = 2;
    TEST_ASSERT_TRUE(bring_up());
    TEST_ASSERT_EQUAL_INT(3, calls_of(CALL_POLL));
    TEST_ASSERT_EQUAL_INT(1, calls_of(CALL_CONFIGURE));
}

static void
test_bringup_gives_up_without_configuring(void) {
    new_sensor(TEST_BRINGUP_CALLS_MAX);
    TEST_ASSERT_FALSE(bring_up());
    TEST_ASSERT_EQUAL_INT(0, calls_of(CALL_CONFIGURE));
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(IMU_RESET_TIMEOUT_US, sensor->now_us);
    TEST_ASSERT_LESS_THAN_INT64(IMU_RESET_TIMEOUT_US + IMU_RESET_SETTLE_US + IMU_RESET_POLL_US, sensor->now_us);
}

static void
test_bringup_reports_a_failed_configure(void) {
    new_sensor(0);
    sensor->configure_fails = true;
    TEST_ASSERT_FALSE(bring_up());
}

void
run_imu_odr_suite(void) {
    RUN_TEST(test_nothing_asked_touches_nothing);
    RUN_TEST(test_switch_applies_the_request);
    RUN_TEST(test_failed_switch_restores_the_old_rate);
    RUN_TEST(test_failed_restore_is_lost);
    RUN_TEST(test_bringup_resets_waits_then_configures);
    RUN_TEST(test_bringup_tolerates_nacks_while_resetting);
    RUN_TEST(test_bringup_gives_up_without_configuring);
    RUN_TEST(test_bringup_reports_a_failed_configure);
}

SUITE_REGISTER(run_imu_odr_suite);

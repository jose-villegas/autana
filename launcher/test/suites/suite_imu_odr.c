/* imu_odr: a requested sensor rate switches, or falls back to the rate that ran. */
#include "suites.h"
#include "unity.h"

#include "input/motion/imu.h"
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

void
run_imu_odr_suite(void) {
    RUN_TEST(test_nothing_asked_touches_nothing);
    RUN_TEST(test_switch_applies_the_request);
    RUN_TEST(test_failed_switch_restores_the_old_rate);
    RUN_TEST(test_failed_restore_is_lost);
}

SUITE_REGISTER(run_imu_odr_suite);

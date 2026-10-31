#include "input/motion/imu_demand.h"
#include "suites.h"
#include "unity.h"

#define DEMAND_TEST_NOW_US ((int64_t)10 * IMU_DEMAND_IDLE_US)

static imu_demand_level_t
level_after(int64_t newest_age_us, int64_t fifo_age_us) {
    const imu_demand_t demand = {
        .newest_us = newest_age_us < 0 ? IMU_DEMAND_NEVER : DEMAND_TEST_NOW_US - newest_age_us,
        .fifo_us = fifo_age_us < 0 ? IMU_DEMAND_NEVER : DEMAND_TEST_NOW_US - fifo_age_us,
    };
    return imu_demand_level(&demand, DEMAND_TEST_NOW_US);
}

static void
test_never_read_parks(void) {
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_NONE, level_after(-1, -1));
}

static void
test_each_kind_lasts_the_idle_window(void) {
    const int64_t last_in = IMU_DEMAND_IDLE_US - 1;
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_NEWEST, level_after(0, -1));
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_NEWEST, level_after(last_in, -1));
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_NONE, level_after(IMU_DEMAND_IDLE_US, -1));
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_FIFO, level_after(-1, 0));
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_FIFO, level_after(-1, last_in));
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_NONE, level_after(-1, IMU_DEMAND_IDLE_US));
}

static void
test_fifo_outranks_newest(void) {
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_FIFO, level_after(0, 0));
    TEST_ASSERT_EQUAL_INT(IMU_DEMAND_NEWEST, level_after(0, IMU_DEMAND_IDLE_US));
}

void
run_imu_demand_suite(void) {
    RUN_TEST(test_never_read_parks);
    RUN_TEST(test_each_kind_lasts_the_idle_window);
    RUN_TEST(test_fifo_outranks_newest);
}

SUITE_REGISTER(run_imu_demand_suite);

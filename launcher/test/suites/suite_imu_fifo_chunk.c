#include "suites.h"
#include "unity.h"

#include <stddef.h>

#include "input/motion/imu_fifo_chunk.h"

static void
check_chunks(int count) {
    int total = 0;
    TEST_ASSERT_EQUAL_INT(0, imu_fifo_chunk_samples(0));
    while (total < count) {
        const int chunk = imu_fifo_chunk_samples(count - total);
        TEST_ASSERT_GREATER_THAN_INT(0, chunk);
        TEST_ASSERT_LESS_OR_EQUAL_INT(IMU_FIFO_CHUNK_SAMPLES, chunk);
        TEST_ASSERT_LESS_OR_EQUAL_INT(count - total, chunk);
        const size_t bus_us = (size_t)chunk * IMU_FIFO_SAMPLE_BUS_US;
        TEST_ASSERT_TRUE(bus_us <= IMU_BUS_HOLD_TARGET_US
                         || (chunk == 1 && IMU_FIFO_SAMPLE_BUS_US > IMU_BUS_HOLD_TARGET_US));
        total += chunk;
    }
    TEST_ASSERT_EQUAL_INT(count, total);
}

static void
test_boundary_counts_partition_into_bounded_chunks(void) {
    const int counts[] = {
        0, 1, IMU_FIFO_CHUNK_SAMPLES - 1, IMU_FIFO_CHUNK_SAMPLES, IMU_FIFO_CHUNK_SAMPLES + 1, IMU_FIFO_SAMPLES};
    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
        check_chunks(counts[i]);
    }
}

void
run_imu_fifo_chunk_suite(void) {
    RUN_TEST(test_boundary_counts_partition_into_bounded_chunks);
}

SUITE_REGISTER(run_imu_fifo_chunk_suite);

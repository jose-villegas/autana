#include <stdlib.h>

#include "input/motion/imu_queue.h"
#include "suites.h"
#include "unity.h"

typedef struct {
    imu_queue_t queue;
    imu_sample_t samples[IMU_FIFO_SAMPLES];
    uint8_t lost[IMU_FIFO_SAMPLES];
    bool gap;
} fixture_t;

static fixture_t*
fixture(void) {
    fixture_t* f = calloc(1, sizeof *f);
    TEST_ASSERT_NOT_NULL(f);
    return f;
}

static void
test_loss_stays_at_each_drain_boundary(void) {
    fixture_t* f = fixture();
    const imu_sample_t first = {.ax = 1};
    const imu_sample_t second = {.ax = 2};
    imu_queue_push(&f->queue, &first, 1, 2, false);
    imu_queue_push(&f->queue, &second, 1, 3, false);
    TEST_ASSERT_EQUAL_INT(2, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_EQUAL_INT(first.ax, f->samples[0].ax);
    TEST_ASSERT_EQUAL_INT(second.ax, f->samples[1].ax);
    TEST_ASSERT_EQUAL_UINT8(2, f->lost[0]);
    TEST_ASSERT_EQUAL_UINT8(3, f->lost[1]);
    TEST_ASSERT_FALSE(f->gap);
    TEST_ASSERT_EQUAL_INT(0, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_FALSE(f->gap);
    free(f);
}

static void
test_empty_drain_carries_loss_across_take(void) {
    fixture_t* f = fixture();
    imu_queue_push(&f->queue, NULL, 0, 2, false);
    TEST_ASSERT_EQUAL_INT(0, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    const imu_sample_t sample = {.ax = 1};
    imu_queue_push(&f->queue, &sample, 1, 3, false);
    TEST_ASSERT_EQUAL_INT(1, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_EQUAL_UINT8(5, f->lost[0]);
    free(f);
}

static void
test_loss_saturation_marks_gap_and_clears_carry(void) {
    fixture_t* f = fixture();
    const imu_sample_t sample = {0};
    imu_queue_push(&f->queue, NULL, 0, UINT8_MAX, false);
    imu_queue_push(&f->queue, &sample, 1, 1, false);
    TEST_ASSERT_EQUAL_INT(1, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_TRUE(f->gap);
    TEST_ASSERT_EQUAL_UINT8(0, f->lost[0]);
    TEST_ASSERT_EQUAL_INT(0, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_FALSE(f->gap);
    free(f);
}

static void
test_overflow_drops_oldest_and_marks_gap(void) {
    fixture_t* f = fixture();
    for (int i = 0; i <= IMU_FIFO_SAMPLES; i++) {
        const imu_sample_t sample = {.ax = (int16_t)i};
        imu_queue_push(&f->queue, &sample, 1, 0, false);
    }
    TEST_ASSERT_EQUAL_INT(IMU_FIFO_SAMPLES, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_TRUE(f->gap);
    for (int i = 0; i < IMU_FIFO_SAMPLES; i++) {
        TEST_ASSERT_EQUAL_INT(i + 1, f->samples[i].ax);
    }
    free(f);
}

static void
test_gap_clears_carried_loss(void) {
    fixture_t* f = fixture();
    const imu_sample_t sample = {0};
    imu_queue_push(&f->queue, NULL, 0, UINT8_MAX, false);
    imu_queue_push(&f->queue, NULL, 0, 0, true);
    imu_queue_push(&f->queue, &sample, 1, 1, false);
    TEST_ASSERT_EQUAL_INT(1, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_TRUE(f->gap);
    TEST_ASSERT_EQUAL_UINT8(1, f->lost[0]);
    free(f);
}

void
run_imu_queue_suite(void) {
    RUN_TEST(test_loss_stays_at_each_drain_boundary);
    RUN_TEST(test_empty_drain_carries_loss_across_take);
    RUN_TEST(test_loss_saturation_marks_gap_and_clears_carry);
    RUN_TEST(test_overflow_drops_oldest_and_marks_gap);
    RUN_TEST(test_gap_clears_carried_loss);
}

SUITE_REGISTER(run_imu_queue_suite);

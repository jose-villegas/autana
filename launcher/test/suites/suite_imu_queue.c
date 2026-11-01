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

static void
test_loss_saturation_boundaries(void) {
    fixture_t* f = fixture();
    const imu_sample_t sample = {0};
    const int losses[] = {UINT8_MAX, UINT8_MAX + 1, UINT8_MAX * IMU_FIFO_SAMPLES};
    for (unsigned i = 0; i < sizeof losses / sizeof losses[0]; i++) {
        imu_queue_push(&f->queue, &sample, 1, losses[i], false);
        TEST_ASSERT_EQUAL_INT(1, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
        TEST_ASSERT_EQUAL_INT(losses[i] > UINT8_MAX, f->gap);
        TEST_ASSERT_EQUAL_UINT8(losses[i] > UINT8_MAX ? 0 : UINT8_MAX, f->lost[0]);
    }
    imu_queue_push(&f->queue, NULL, 0, 1, false);
    imu_queue_push(&f->queue, &sample, 1, UINT8_MAX - 1, false);
    TEST_ASSERT_EQUAL_INT(1, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_FALSE(f->gap);
    TEST_ASSERT_EQUAL_UINT8(UINT8_MAX, f->lost[0]);
    free(f);
}

static void
test_wrapped_loss_stays_with_sample(void) {
    fixture_t* f = fixture();
    for (int i = 0; i < IMU_FIFO_SAMPLES + 3; i++) {
        const imu_sample_t sample = {.ax = (int16_t)i};
        imu_queue_push(&f->queue, &sample, 1, i + 1, false);
    }
    TEST_ASSERT_NOT_EQUAL(0, f->queue.head);
    TEST_ASSERT_EQUAL_INT(IMU_FIFO_SAMPLES, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_TRUE(f->gap);
    for (int i = 0; i < IMU_FIFO_SAMPLES; i++) {
        TEST_ASSERT_EQUAL_INT(i + 3, f->samples[i].ax);
        TEST_ASSERT_EQUAL_UINT8(i + 4, f->lost[i]);
    }
    free(f);
}

static void
test_bulk_overflow_preserves_order_and_single_loss_boundary(void) {
    fixture_t* f = fixture();
    const int count = IMU_FIFO_SAMPLES + 3;
    imu_sample_t* samples = calloc((size_t)count, sizeof *samples);
    TEST_ASSERT_NOT_NULL(samples);
    for (int i = 0; i < count; i++) {
        samples[i].ax = (int16_t)i;
    }
    imu_queue_push(&f->queue, samples, count, 7, false);
    TEST_ASSERT_EQUAL_INT(IMU_FIFO_SAMPLES, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_TRUE(f->gap);
    for (int i = 0; i < IMU_FIFO_SAMPLES; i++) {
        TEST_ASSERT_EQUAL_INT(i + 3, f->samples[i].ax);
        TEST_ASSERT_EQUAL_UINT8(0, f->lost[i]);
    }
    imu_queue_push(&f->queue, samples, IMU_FIFO_SAMPLES - 1, 0, false);
    imu_queue_push(&f->queue, samples, 3, 7, false);
    TEST_ASSERT_EQUAL_INT(IMU_FIFO_SAMPLES, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_TRUE(f->gap);
    for (int i = 0; i < IMU_FIFO_SAMPLES; i++) {
        const int boundary = IMU_FIFO_SAMPLES - 3;
        TEST_ASSERT_EQUAL_INT(i < boundary ? i + 2 : i - boundary, f->samples[i].ax);
        TEST_ASSERT_EQUAL_UINT8(i == boundary ? 7 : 0, f->lost[i]);
    }
    free(samples);
    free(f);
}

static void
test_empty_gap_is_taken_once(void) {
    fixture_t* f = fixture();
    imu_queue_push(&f->queue, NULL, 0, 0, true);
    TEST_ASSERT_EQUAL_INT(0, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_TRUE(f->gap);
    TEST_ASSERT_EQUAL_INT(0, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_FALSE(f->gap);
    free(f);
}

static void
test_gap_discards_preceding_samples(void) {
    fixture_t* f = fixture();
    const imu_sample_t sample = {.ax = 1};
    imu_queue_push(&f->queue, &sample, 1, 0, false);
    imu_queue_push(&f->queue, &sample, 1, 0, false);
    imu_queue_push(&f->queue, NULL, 0, 0, true);
    const imu_sample_t next = {.ax = 2};
    imu_queue_push(&f->queue, &next, 1, 0, false);
    TEST_ASSERT_EQUAL_INT(1, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_TRUE(f->gap);
    TEST_ASSERT_EQUAL_INT(next.ax, f->samples[0].ax);
    free(f);
}

static void
test_drain_result_maps_failure_and_clamps_count(void) {
    fixture_t* f = fixture();
    imu_queue_push_drain(&f->queue, NULL, -1, 0, false);
    TEST_ASSERT_EQUAL_INT(0, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_TRUE(f->gap);
    imu_queue_push_drain(&f->queue, f->samples, IMU_FIFO_SAMPLES + 1, 0, false);
    TEST_ASSERT_EQUAL_INT(IMU_FIFO_SAMPLES, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
    TEST_ASSERT_FALSE(f->gap);
    free(f);
}

/* One sample pushed, then a take at `odr_code`: its count, with the gap flag in f->gap. */
static int
push_then_take(fixture_t* f, bool push, unsigned odr_code) {
    const imu_sample_t sample = {.ax = 1};
    if (push) {
        imu_queue_push(&f->queue, &sample, 1, 0, false);
    }
    return imu_queue_take_at(&f->queue, odr_code, f->samples, f->lost, &f->gap);
}

/* Samples at another rate never reach a default-rate reader: gravity would integrate them
 * at IMU_SAMPLE_US. The rate's own reader takes them, or finds them gone behind a gap. */
static void
test_queued_rate_decides_who_takes(void) {
    fixture_t* f = fixture();

    static const struct {
        bool switch_rate, push;
        unsigned odr_code;
        int taken;
    } steps[] = {
        {true, true, IMU_BUZZ_ODR_CODE, 1},   /* the switch dropped what came before */
        {false, true, IMU_ODR_CODE, 0},       /* a default reader gets nothing */
        {false, false, IMU_BUZZ_ODR_CODE, 0}, /* and the buzz reader sees its sample gone */
        {true, true, IMU_ODR_CODE, 1},        /* back at the default rate */
    };

    (void)push_then_take(f, true, IMU_ODR_CODE);
    unsigned rate = IMU_ODR_CODE;
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        if (steps[i].switch_rate) {
            rate = rate == IMU_ODR_CODE ? IMU_BUZZ_ODR_CODE : IMU_ODR_CODE;
            imu_queue_set_rate(&f->queue, rate);
        }
        TEST_ASSERT_EQUAL_INT(steps[i].taken, push_then_take(f, steps[i].push, steps[i].odr_code));
        TEST_ASSERT_TRUE(f->gap);
    }
    free(f);
}

static void
test_restart_gap_is_taken_before_next_drain(void) {
    fixture_t* f = fixture();
    const imu_sample_t stale = {.ax = IMU_COUNTS_PER_G};
    const imu_sample_t fresh = {.ay = IMU_COUNTS_PER_G};
    for (unsigned before_drain = 0; before_drain <= 1; before_drain++) {
        bool fifo_gap = true;
        int pending_lost = 1;
        imu_queue_push(&f->queue, &stale, 1, pending_lost, false);
        imu_queue_restart(&f->queue, &fifo_gap, &pending_lost);
        TEST_ASSERT_EQUAL_INT(0, pending_lost);
        if (before_drain) {
            TEST_ASSERT_EQUAL_INT(0, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
            TEST_ASSERT_TRUE(f->gap);
        }
        imu_queue_push_drain(&f->queue, &fresh, 1, pending_lost, fifo_gap);
        TEST_ASSERT_EQUAL_INT(1, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
        TEST_ASSERT_EQUAL_INT(fresh.ay, f->samples[0].ay);
        TEST_ASSERT_EQUAL_UINT8(0, f->lost[0]);
        TEST_ASSERT_EQUAL_INT(!before_drain, f->gap);
        TEST_ASSERT_EQUAL_INT(0, imu_queue_take(&f->queue, f->samples, f->lost, &f->gap));
        TEST_ASSERT_FALSE(f->gap);
    }
    free(f);
}

void
run_imu_queue_suite(void) {
    RUN_TEST(test_restart_gap_is_taken_before_next_drain);
    RUN_TEST(test_loss_stays_at_each_drain_boundary);
    RUN_TEST(test_empty_drain_carries_loss_across_take);
    RUN_TEST(test_loss_saturation_marks_gap_and_clears_carry);
    RUN_TEST(test_overflow_drops_oldest_and_marks_gap);
    RUN_TEST(test_gap_clears_carried_loss);
    RUN_TEST(test_loss_saturation_boundaries);
    RUN_TEST(test_wrapped_loss_stays_with_sample);
    RUN_TEST(test_bulk_overflow_preserves_order_and_single_loss_boundary);
    RUN_TEST(test_empty_gap_is_taken_once);
    RUN_TEST(test_gap_discards_preceding_samples);
    RUN_TEST(test_drain_result_maps_failure_and_clamps_count);
    RUN_TEST(test_queued_rate_decides_who_takes);
}

SUITE_REGISTER(run_imu_queue_suite);

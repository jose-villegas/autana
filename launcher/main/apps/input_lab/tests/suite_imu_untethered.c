#include "suites.h"
#include "unity.h"

#include <stdlib.h>
#include <string.h>

#include "apps/input_lab/imu_record.h"

#define TEST_DT_MS          20U
#define TEST_SMALL_CAPACITY (sizeof IMU_RECORD_TRUNCATED + 8)
#define TEST_LINE           IMU_RECORD_PREFIX " round imu quarter 1\n"

/* The callback is synchronous; only the test owns this pointer. */
static const char* expected_text;

static input_motion_t
fixture(void) {
    return (input_motion_t){.acceleration = {0, 0, -INPUT_ACCEL_ONE_G}};
}

static void
check_replay(const char* text) {
    TEST_ASSERT_EQUAL_STRING(expected_text, text);
}

static void
test_record_replay_and_release(void) {
    imu_record_t record = {0};
    TEST_ASSERT_TRUE(imu_record_start(&record, IMU_RECORD_CAPACITY));
    TEST_ASSERT_TRUE(imu_record_append(&record, TEST_LINE));
    TEST_ASSERT_TRUE(imu_record_append(&record, TEST_LINE));
    expected_text = TEST_LINE TEST_LINE;
    imu_record_replay(&record, check_replay);
    TEST_ASSERT_NULL(record.text);
    TEST_ASSERT_EQUAL_UINT(0, record.used);
    imu_record_free(&record);
}

static void
test_record_overflow_and_restart(void) {
    imu_record_t record = {0};
    TEST_ASSERT_FALSE(imu_record_start(&record, sizeof IMU_RECORD_TRUNCATED - 1));
    TEST_ASSERT_TRUE(imu_record_start(&record, TEST_SMALL_CAPACITY));
    TEST_ASSERT_TRUE(imu_record_append(&record, "1234567\n"));
    TEST_ASSERT_FALSE(imu_record_append(&record, TEST_LINE));
    TEST_ASSERT_FALSE(imu_record_append(&record, "ignored\n"));
    TEST_ASSERT_LESS_THAN(record.capacity, record.used);
    expected_text = "1234567\n" IMU_RECORD_TRUNCATED;
    imu_record_replay(&record, check_replay);
    TEST_ASSERT_TRUE(imu_record_start(&record, TEST_SMALL_CAPACITY));
    TEST_ASSERT_TRUE(imu_record_append(&record, "old\n"));
    TEST_ASSERT_TRUE(imu_record_start(&record, TEST_SMALL_CAPACITY));
    expected_text = "";
    imu_record_replay(&record, check_replay);
}

static void
test_pose_upright_turn_order(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    const vec3f_t down = {0, 0, -1};
    input_motion_t sample = fixture();
    sample.rotation.x = IMU_PROBE_TURN_THRESHOLD + 1;
    TEST_ASSERT_FALSE(imu_probe_turn_update(round, &sample, down, TEST_DT_MS));
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_POSES, round->step);
    sample.rotation.x = 0;
    for (int pose = 0; pose < IMU_PROBE_POSE_COUNT; pose++) {
        sample.acceleration = vec3i_scale(imu_probe_pose_direction(pose, 0), INPUT_ACCEL_ONE_G);
        for (unsigned ms = 0; ms <= IMU_PROBE_SETTLE_MS + IMU_PROBE_WINDOW_MS; ms += TEST_DT_MS) {
            imu_probe_update(round, &sample, TEST_DT_MS);
        }
    }
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_UPRIGHT, round->step);
    sample = fixture();
    for (unsigned ms = 0; ms <= IMU_PROBE_SETTLE_MS; ms += TEST_DT_MS) {
        TEST_ASSERT_FALSE(imu_probe_turn_update(round, &sample, down, TEST_DT_MS));
    }
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_UPRIGHT, round->step);
    sample.acceleration = vec3i_scale(imu_probe_pose_direction(IMU_PROBE_BOTTOM_DOWN, 0), INPUT_ACCEL_ONE_G);
    for (unsigned ms = 0; ms <= IMU_PROBE_SETTLE_MS + TEST_DT_MS; ms += TEST_DT_MS) {
        TEST_ASSERT_FALSE(imu_probe_turn_update(round, &sample, down, TEST_DT_MS));
    }
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_TURN, round->step);
    free(round);
}

static void
test_turn_threshold_window_and_decimation(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, 0);
    round->step = IMU_PROBE_TURN;
    round->pose = IMU_PROBE_POSE_COUNT;
    input_motion_t sample = fixture();
    vec3f_t down = {1, 0, 0};
    sample.rotation.y = -IMU_PROBE_TURN_THRESHOLD;
    TEST_ASSERT_FALSE(imu_probe_turn_update(round, &sample, down, TEST_DT_MS));
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_TURN, round->step);
    sample.rotation.y--;
    TEST_ASSERT_FALSE(imu_probe_turn_update(round, NULL, down, TEST_DT_MS));
    TEST_ASSERT_FALSE(imu_probe_turn_update(round, &sample, down, 0));
    TEST_ASSERT_TRUE(imu_probe_turn_update(round, &sample, down, TEST_DT_MS));
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_TRACE, round->step);
    TEST_ASSERT_EQUAL_UINT32(0, round->trace_ms);
    TEST_ASSERT_EQUAL_INT(1, round->trace_count);
    TEST_ASSERT_EQUAL_FLOAT(down.x, round->trace_down.x);
    sample.rotation.y = 0;
    for (unsigned ms = TEST_DT_MS; ms < IMU_PROBE_TURN_WINDOW_MS; ms += TEST_DT_MS) {
        imu_probe_turn_update(round, &sample, down, TEST_DT_MS);
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_TRACE, round->step);
    }
    down = (vec3f_t){0, 1, 0};
    TEST_ASSERT_TRUE(imu_probe_turn_update(round, &sample, down, TEST_DT_MS));
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_DONE, round->step);
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_TURN_SAMPLES, round->trace_count);
    TEST_ASSERT_EQUAL_UINT32(IMU_PROBE_TURN_WINDOW_MS, round->trace_ms);
    TEST_ASSERT_EQUAL_FLOAT(down.y, round->trace_down.y);
    TEST_ASSERT_FALSE(imu_probe_turn_update(round, &sample, down, TEST_DT_MS));
    free(round);
}

void
run_imu_untethered_suite(void) {
    RUN_TEST(test_record_replay_and_release);
    RUN_TEST(test_record_overflow_and_restart);
    RUN_TEST(test_pose_upright_turn_order);
    RUN_TEST(test_turn_threshold_window_and_decimation);
}

SUITE_REGISTER(run_imu_untethered_suite);

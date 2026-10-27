#include "suites.h"
#include "unity.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps/input_lab/imu_record.h"
#include "display/display_judge.h"

#define TEST_DT_MS          20U
#define TEST_SMALL_CAPACITY (sizeof IMU_RECORD_TRUNCATED + sizeof(IMU_RECORD_PREFIX " 1234567\n") - 1)
#define TEST_LINE           IMU_RECORD_PREFIX " round imu quarter 1\n"

/* The callback is synchronous; only the test owns this pointer. */
static const char* expected_text;
static size_t replay_calls;
static size_t replay_bytes;

static input_motion_t
fixture(void) {
    return (input_motion_t){.acceleration = {0, 0, -INPUT_ACCEL_ONE_G}};
}

static void
check_replay(const char* text, size_t length) {
    replay_calls++;
    TEST_ASSERT_EQUAL_UINT(strlen(expected_text), length);
    TEST_ASSERT_EQUAL_MEMORY(expected_text, text, length);
}

static void
test_record_replay_retains_text(void) {
    imu_record_t record = {0};
    TEST_ASSERT_TRUE(imu_record_start(&record, IMU_RECORD_CAPACITY));
    TEST_ASSERT_TRUE(imu_record_appendf(&record, "round imu quarter %d", 1));
    TEST_ASSERT_TRUE(imu_record_appendf(&record, "round imu quarter %d", 1));
    expected_text = TEST_LINE TEST_LINE;
    replay_calls = 0;
    imu_record_replay(&record, check_replay);
    imu_record_replay(&record, check_replay);
    TEST_ASSERT_EQUAL_UINT(2, replay_calls);
    TEST_ASSERT_NOT_NULL(record.text);
    TEST_ASSERT_EQUAL_STRING(expected_text, record.text);
    imu_record_free(&record);
}

static void
check_chunk(const char* text, size_t length) {
    TEST_ASSERT_GREATER_THAN(0, length);
    TEST_ASSERT_LESS_OR_EQUAL(IMU_RECORD_CHUNK_SIZE, length);
    TEST_ASSERT_EQUAL_CHAR('\n', text[length - 1]);
    for (size_t i = 0; i < length; i++) {
        TEST_ASSERT_EQUAL_CHAR(TEST_LINE[(replay_bytes + i) % (sizeof TEST_LINE - 1)], text[i]);
    }
    replay_bytes += length;
    replay_calls++;
}

static void
test_record_replay_chunks_whole_lines(void) {
    imu_record_t record = {0};
    TEST_ASSERT_TRUE(imu_record_start(&record, (3 * IMU_RECORD_CHUNK_SIZE) + sizeof IMU_RECORD_TRUNCATED));
    const size_t lines = (3 * IMU_RECORD_CHUNK_SIZE) / (sizeof TEST_LINE - 1);
    for (size_t i = 0; i < lines; i++) {
        TEST_ASSERT_TRUE(imu_record_appendf(&record, "round imu quarter %d", 1));
    }
    for (int repeat = 0; repeat < 2; repeat++) {
        replay_bytes = 0;
        replay_calls = 0;
        imu_record_replay(&record, check_chunk);
        TEST_ASSERT_EQUAL_UINT(record.used, replay_bytes);
        TEST_ASSERT_GREATER_THAN(1, replay_calls);
    }
    imu_record_free(&record);
}

static void
check_exact_chunk(const char* text, size_t length) {
    TEST_ASSERT_EQUAL_UINT(IMU_RECORD_CHUNK_SIZE, length);
    TEST_ASSERT_EQUAL_CHAR('\n', text[length - 1]);
    TEST_ASSERT_EQUAL_MEMORY(expected_text + replay_bytes, text, length);
    replay_bytes += length;
    replay_calls++;
}

static void
test_record_replay_null_empty_and_exact_chunks(void) {
    imu_record_t record = {0};
    replay_calls = 0;
    imu_record_replay(NULL, check_replay);
    imu_record_replay(&record, check_replay);
    TEST_ASSERT_EQUAL_UINT(0, replay_calls);
    const size_t length = 2 * IMU_RECORD_CHUNK_SIZE;
    TEST_ASSERT_TRUE(imu_record_start(&record, length + sizeof IMU_RECORD_TRUNCATED));
    for (size_t i = 0; i < length / sizeof(IMU_RECORD_PREFIX " 1234567"); i++) {
        TEST_ASSERT_TRUE(imu_record_appendf(&record, "1234567"));
    }
    TEST_ASSERT_EQUAL_UINT(length, record.used);
    expected_text = record.text;
    replay_bytes = 0;
    imu_record_replay(&record, check_exact_chunk);
    TEST_ASSERT_EQUAL_UINT(2, replay_calls);
    TEST_ASSERT_EQUAL_UINT(length, replay_bytes);
    imu_record_free(&record);
}

static void
test_record_overflow_and_restart(void) {
    imu_record_t record = {0};
    TEST_ASSERT_FALSE(imu_record_start(&record, sizeof IMU_RECORD_TRUNCATED - 1));
    TEST_ASSERT_TRUE(imu_record_start(&record, TEST_SMALL_CAPACITY));
    TEST_ASSERT_TRUE(imu_record_appendf(&record, "1234567"));
    TEST_ASSERT_FALSE(imu_record_appendf(&record, "round imu quarter %d", 1));
    TEST_ASSERT_FALSE(imu_record_appendf(&record, "ignored"));
    TEST_ASSERT_LESS_THAN(record.capacity, record.used);
    expected_text = IMU_RECORD_PREFIX " 1234567\n" IMU_RECORD_TRUNCATED;
    replay_calls = 0;
    imu_record_replay(&record, check_replay);
    TEST_ASSERT_EQUAL_UINT(1, replay_calls);
    TEST_ASSERT_TRUE(imu_record_start(&record, TEST_SMALL_CAPACITY));
    TEST_ASSERT_TRUE(imu_record_appendf(&record, "old"));
    TEST_ASSERT_TRUE(imu_record_start(&record, TEST_SMALL_CAPACITY));
    TEST_ASSERT_EQUAL_STRING("", record.text);
    TEST_ASSERT_EQUAL_UINT(0, record.used);
    imu_record_free(&record);
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
        for (unsigned ms = 0; ms <= IMU_REST_SETTLE_MS + IMU_REST_WINDOW_MS; ms += TEST_DT_MS) {
            imu_probe_update(round, &sample, TEST_DT_MS);
        }
    }
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_UPRIGHT, round->step);
    sample = fixture();
    for (unsigned ms = 0; ms <= IMU_REST_SETTLE_MS; ms += TEST_DT_MS) {
        TEST_ASSERT_FALSE(imu_probe_turn_update(round, &sample, down, TEST_DT_MS));
    }
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_UPRIGHT, round->step);
    sample.acceleration = vec3i_scale(imu_probe_pose_direction(IMU_PROBE_BOTTOM_DOWN, 0), INPUT_ACCEL_ONE_G);
    for (unsigned ms = 0; ms <= IMU_REST_SETTLE_MS + TEST_DT_MS; ms += TEST_DT_MS) {
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

static void
test_record_boundaries_and_dump_decision(void) {
    imu_record_t record = {0};
    TEST_ASSERT_NOT_NULL(imu_record_dump_error(&record, false));
    TEST_ASSERT_TRUE(imu_record_start(&record, TEST_SMALL_CAPACITY));
    TEST_ASSERT_EQUAL_STRING(IMU_RECORD_PREFIX "_ERR round active\n", imu_record_dump_error(&record, true));
    TEST_ASSERT_NULL(imu_record_dump_error(&record, false));
    TEST_ASSERT_TRUE(imu_record_appendf(&record, "1234567"));
    TEST_ASSERT_FALSE(imu_record_appendf(&record, ""));
    TEST_ASSERT_TRUE(imu_record_start(&record, TEST_SMALL_CAPACITY));
    TEST_ASSERT_FALSE(imu_record_appendf(&record, "12345678"));
    TEST_ASSERT_EQUAL_STRING(IMU_RECORD_TRUNCATED, record.text);
    TEST_ASSERT_TRUE(imu_record_start(&record, sizeof IMU_RECORD_TRUNCATED));
    TEST_ASSERT_FALSE(imu_record_appendf(&record, ""));
    TEST_ASSERT_EQUAL_STRING(IMU_RECORD_TRUNCATED, record.text);
    TEST_ASSERT_FALSE(imu_record_appendf(&record, "again"));
    TEST_ASSERT_EQUAL_STRING(IMU_RECORD_TRUNCATED, record.text);
    TEST_ASSERT_TRUE(imu_record_start(&record, IMU_RECORD_CAPACITY));
    const char* format = "calib x off %d scale %d y off %d scale %d z off %d scale %d gyro %d,%d,%d";
    char payload[IMU_RECORD_LINE_SIZE];
    const int length = snprintf(payload, sizeof payload, format, INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN, INT_MIN,
                                INT_MIN, INT_MIN, INT_MIN);
    TEST_ASSERT_LESS_OR_EQUAL(IMU_RECORD_FIT_SIZE, length);
    TEST_ASSERT_TRUE(imu_record_appendf(&record, "%s", payload));
    TEST_ASSERT_EQUAL_UINT(strlen(payload) + sizeof(IMU_RECORD_PREFIX " "), record.used);
    const size_t exact_payload = IMU_RECORD_LINE_SIZE - sizeof(IMU_RECORD_PREFIX " ") - 1;
    memset(payload, 'x', exact_payload + 1);
    payload[exact_payload] = '\0';
    TEST_ASSERT_TRUE(imu_record_appendf(&record, "%s", payload));
    payload[exact_payload] = 'x';
    payload[exact_payload + 1] = '\0';
    TEST_ASSERT_FALSE(imu_record_appendf(&record, "%s", payload));
    TEST_ASSERT_TRUE(record.truncated);
    TEST_ASSERT_TRUE(imu_record_start(&record, IMU_RECORD_CAPACITY));
    memset(payload, 'x', sizeof payload - 1);
    payload[sizeof payload - 1] = '\0';
    TEST_ASSERT_FALSE(imu_record_appendf(&record, "%s", payload));
    TEST_ASSERT_FALSE(imu_record_appendf(&record, "again"));
    imu_record_free(&record);
}

static imu_probe_round_t*
upright_fixture(int quarter, input_motion_t* rest) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    imu_probe_start(round, quarter);
    round->step = IMU_PROBE_UPRIGHT;
    round->pose = IMU_PROBE_POSE_COUNT;
    *rest = (input_motion_t){
        .acceleration = vec3i_scale(imu_probe_pose_direction(IMU_PROBE_BOTTOM_DOWN, quarter), INPUT_ACCEL_ONE_G)};
    for (unsigned ms = 0; ms <= IMU_REST_SETTLE_MS / 2; ms += TEST_DT_MS) {
        imu_probe_turn_update(round, rest, (vec3f_t){0, 0, -1}, TEST_DT_MS);
    }
    TEST_ASSERT_TRUE(round->turn_ms > 0);
    return round;
}

static void
check_upright_restart(imu_probe_round_t* round, input_motion_t rest, const input_motion_t* disturbed, uint32_t dt_ms) {
    imu_probe_turn_update(round, disturbed, (vec3f_t){0, 0, -1}, dt_ms);
    TEST_ASSERT_EQUAL_UINT32(0, round->turn_ms);
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_UPRIGHT, round->step);
    for (unsigned ms = 0; ms < IMU_REST_SETTLE_MS / 2; ms += TEST_DT_MS) {
        imu_probe_turn_update(round, &rest, (vec3f_t){0, 0, -1}, TEST_DT_MS);
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_UPRIGHT, round->step);
    }
    for (unsigned ms = 0; ms < IMU_REST_SETTLE_MS * 2; ms += TEST_DT_MS) {
        imu_probe_turn_update(round, &rest, (vec3f_t){0, 0, -1}, TEST_DT_MS);
    }
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_TURN, round->step);
    free(round);
}

static void
test_upright_turn_restarts_settle(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        input_motion_t rest;
        imu_probe_round_t* round = upright_fixture(quarter, &rest);
        input_motion_t disturbed = rest;
        disturbed.rotation.x = IMU_PROBE_TURN_THRESHOLD + 1;
        check_upright_restart(round, rest, &disturbed, TEST_DT_MS);
    }
}

static void
test_upright_gravity_mismatch_restarts_settle(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        input_motion_t rest;
        imu_probe_round_t* round = upright_fixture(quarter, &rest);
        input_motion_t disturbed = rest;
        disturbed.acceleration = vec3i_scale(rest.acceleration, -1);
        check_upright_restart(round, rest, &disturbed, TEST_DT_MS);
    }
}

static void
test_upright_null_sample_restarts_settle(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        input_motion_t rest;
        imu_probe_round_t* round = upright_fixture(quarter, &rest);
        check_upright_restart(round, rest, NULL, TEST_DT_MS);
    }
}

static void
test_upright_gap_restarts_settle(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        input_motion_t rest;
        imu_probe_round_t* round = upright_fixture(quarter, &rest);
        check_upright_restart(round, rest, &rest, IMU_REST_MAX_GAP_MS + 1);
    }
}

static void
test_upright_nonzero_quarter_arms(void) {
    for (int quarter = 1; quarter < 4; quarter++) {
        input_motion_t rest;
        imu_probe_round_t* round = upright_fixture(quarter, &rest);
        for (unsigned ms = 0; ms < IMU_REST_SETTLE_MS; ms += TEST_DT_MS) {
            imu_probe_turn_update(round, &rest, (vec3f_t){0, 0, -1}, TEST_DT_MS);
        }
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_TURN, round->step);
        free(round);
    }
}

static void
test_upright_cone_uses_direction(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        input_motion_t rest;
        imu_probe_round_t* round = upright_fixture(quarter, &rest);
        input_motion_t tilted = rest;
        tilted.acceleration.z = INPUT_ACCEL_ONE_G;
        check_upright_restart(round, rest, &tilted, TEST_DT_MS);
        round = upright_fixture(quarter, &rest);
        tilted.acceleration = vec3i_scale(rest.acceleration, 2);
        tilted.acceleration.z = INPUT_ACCEL_ONE_G;
        check_upright_restart(round, tilted, NULL, TEST_DT_MS);
    }
}

static void
test_upright_hand_tremor_arms(void) {
    for (int quarter = 0; quarter < 4; quarter++) {
        input_motion_t rest;
        imu_probe_round_t* round = upright_fixture(quarter, &rest);
        round->turn_ms = 0;
        for (unsigned ms = 0; ms < IMU_REST_SETTLE_MS; ms += TEST_DT_MS) {
            input_motion_t tremor = rest;
            const int sign = (ms / TEST_DT_MS) % 2 ? 1 : -1;
            tremor.acceleration.z = sign * (IMU_REST_ACCEL_DEPARTURE + 1);
            tremor.rotation.x = sign * (IMU_REST_GYRO_DEPARTURE + 1);
            imu_probe_turn_update(round, &tremor, (vec3f_t){0, 0, -1}, TEST_DT_MS);
            if (ms + TEST_DT_MS < IMU_REST_SETTLE_MS) {
                TEST_ASSERT_EQUAL_INT(IMU_PROBE_UPRIGHT, round->step);
            }
        }
        TEST_ASSERT_EQUAL_INT(IMU_PROBE_TURN, round->step);
        free(round);
    }
}

static void
test_trace_missing_reading_deadline_and_period(void) {
    imu_probe_round_t* round = calloc(1, sizeof *round);
    TEST_ASSERT_NOT_NULL(round);
    const input_motion_t sample = fixture();
    const vec3f_t down = {0, 1, 0};
    round->step = IMU_PROBE_TRACE;
    round->trace_count = 1;
    TEST_ASSERT_FALSE(imu_probe_turn_update(round, NULL, down, IMU_PROBE_TURN_PERIOD_MS));
    TEST_ASSERT_EQUAL_UINT32(IMU_PROBE_TURN_PERIOD_MS, round->turn_ms);
    TEST_ASSERT_EQUAL_INT(1, round->trace_count);
    TEST_ASSERT_EQUAL_UINT32(0, round->trace_ms);
    round->turn_ms = 0;
    for (unsigned ms = 1; ms <= IMU_PROBE_TURN_WINDOW_MS; ms++) {
        const bool recorded = imu_probe_turn_update(round, &sample, down, 1);
        TEST_ASSERT_EQUAL_INT(ms % IMU_PROBE_TURN_PERIOD_MS == 0, recorded);
        if (recorded) {
            TEST_ASSERT_EQUAL_UINT32(ms, round->trace_ms);
        }
    }
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_TURN_SAMPLES, round->trace_count);
    round->step = IMU_PROBE_TRACE;
    round->turn_ms = IMU_PROBE_TURN_WINDOW_MS - TEST_DT_MS;
    round->trace_ms = IMU_PROBE_TURN_WINDOW_MS - IMU_PROBE_TURN_PERIOD_MS;
    const int count = round->trace_count;
    TEST_ASSERT_FALSE(imu_probe_turn_update(round, &sample, down, TEST_DT_MS + 1));
    TEST_ASSERT_EQUAL_INT(IMU_PROBE_DONE, round->step);
    TEST_ASSERT_EQUAL_INT(count, round->trace_count);
    free(round);
}

static void
test_orientation_capacity_covers_ready_dwell(void) {
    TEST_ASSERT_GREATER_THAN_UINT(IMU_RECORD_SHAKE_CAPACITY, IMU_RECORD_ORIENTATION_CAPACITY);
    imu_record_t record = {0};
    TEST_ASSERT_TRUE(imu_record_start(&record, IMU_RECORD_ORIENTATION_LINE_SIZE + sizeof IMU_RECORD_TRUNCATED));
    const int acc = ORIENTATION_PROBE_ACCEL_MAX_COUNTS;
    const int gyro = ORIENTATION_PROBE_GYRO_MAX_COUNTS;
    const long long timestamp = (long long)ORIENTATION_PROBE_ROUND_MS * MATH_US_PER_MS;
    for (int sign = -1; sign <= 1; sign += 2) {
        record.used = 0;
        TEST_ASSERT_TRUE(imu_record_appendf(&record, ORIENTATION_PROBE_SAMPLE_FORMAT, (int)ORIENTATION_PROBE_DONE - 1,
                                            timestamp - 1, sign * acc, sign * acc, sign * acc, sign * gyro, sign * gyro,
                                            sign * gyro));
        TEST_ASSERT_LESS_OR_EQUAL_UINT(IMU_RECORD_ORIENTATION_LINE_SIZE, record.used);
    }
    record.used = 0;
    TEST_ASSERT_TRUE(imu_record_appendf(&record, "orientation_step %d %lld %d", (int)ORIENTATION_PROBE_DONE, timestamp,
                                        DISPLAY_QUARTER_NONE));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(IMU_RECORD_ORIENTATION_LINE_SIZE, record.used);
    TEST_ASSERT_FALSE(record.truncated);
    imu_record_free(&record);
}

void
run_imu_untethered_suite(void) {
    RUN_TEST(test_orientation_capacity_covers_ready_dwell);
    RUN_TEST(test_upright_turn_restarts_settle);
    RUN_TEST(test_upright_gravity_mismatch_restarts_settle);
    RUN_TEST(test_upright_null_sample_restarts_settle);
    RUN_TEST(test_upright_gap_restarts_settle);
    RUN_TEST(test_upright_nonzero_quarter_arms);
    RUN_TEST(test_upright_hand_tremor_arms);
    RUN_TEST(test_upright_cone_uses_direction);
    RUN_TEST(test_trace_missing_reading_deadline_and_period);
    RUN_TEST(test_record_boundaries_and_dump_decision);
    RUN_TEST(test_record_replay_retains_text);
    RUN_TEST(test_record_replay_null_empty_and_exact_chunks);
    RUN_TEST(test_record_replay_chunks_whole_lines);
    RUN_TEST(test_record_overflow_and_restart);
    RUN_TEST(test_pose_upright_turn_order);
    RUN_TEST(test_turn_threshold_window_and_decimation);
}

SUITE_REGISTER(run_imu_untethered_suite);

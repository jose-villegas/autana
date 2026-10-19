#include "frame_profile.h"
#include "suites.h"
#include "unity.h"

typedef struct {
    const int64_t* times;
    const int64_t* steps;
    int count;
    int calls;
} fixture_t;

static int64_t
scripted_frame(void* ctx, int64_t dt_us) {
    fixture_t* f = ctx;
    TEST_ASSERT_LESS_THAN_INT(f->count, f->calls);
    TEST_ASSERT_EQUAL_INT64(f->steps[f->calls], dt_us);
    return f->times[f->calls++];
}

static void
test_previous_frame_drives_duration_and_statistics(void) {
    const int64_t times[] = {700, 100, 200, 300};
    const int64_t steps[] = {0, 700, 100, 200};
    fixture_t f = {times, steps, sizeof times / sizeof times[0], 0};
    const frame_profile_scenario_t scenario = {"script", 1000};
    const frame_profile_t p = frame_profile_play(&scenario, scripted_frame, &f);
    TEST_ASSERT_EQUAL_INT(f.count, f.calls);
    TEST_ASSERT_EQUAL_INT(3, p.frames);
    TEST_ASSERT_EQUAL_INT64(100, p.frame_us.min);
    TEST_ASSERT_EQUAL_INT64(300, p.frame_us.max);
    TEST_ASSERT_EQUAL_INT64(200, p.frame_us.avg);
    TEST_ASSERT_EQUAL_INT64(200, p.frame_us.med);
    TEST_ASSERT_EQUAL_INT64(300, p.frame_us.p95);
}

static int64_t
fast_frame(void* ctx, int64_t dt_us) {
    int* calls = ctx;
    (*calls)++;
    return 1;
}

static void
test_sample_capacity_fails_without_overrun(void) {
    int calls = 0;
    const frame_profile_scenario_t scenario = {"too_fast", FRAME_PROFILE_MIN_FRAME_US};
    const frame_profile_t p = frame_profile_play(&scenario, fast_frame, &calls);
    TEST_ASSERT_EQUAL_INT(0, p.frames);
    TEST_ASSERT_EQUAL_INT(3, calls);
}

void
suite_frame_profile(void) {
    RUN_TEST(test_previous_frame_drives_duration_and_statistics);
    RUN_TEST(test_sample_capacity_fails_without_overrun);
}

SUITE_REGISTER(suite_frame_profile);

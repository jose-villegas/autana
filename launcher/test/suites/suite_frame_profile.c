#include <limits.h>
#include "core/memory.h"
#include "frame_profile.h"
#include "heap_arena.h"
#include "perf_budget.h"
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
    const int64_t times[] = {7 * MATH_US_PER_MS, MATH_US_PER_MS, 2 * MATH_US_PER_MS, 3 * MATH_US_PER_MS};
    const int64_t steps[] = {0, 7 * MATH_US_PER_MS, MATH_US_PER_MS, 2 * MATH_US_PER_MS};
    fixture_t f = {times, steps, sizeof times / sizeof times[0], 0};
    const frame_profile_scenario_t scenario = {"script", 10 * MATH_US_PER_MS};
    const frame_profile_t p = frame_profile_play(&scenario, scripted_frame, &f, NULL);
    TEST_ASSERT_EQUAL_INT(f.count, f.calls);
    TEST_ASSERT_EQUAL_INT(3, p.frames);
    TEST_ASSERT_EQUAL_INT64(6 * MATH_US_PER_MS, p.total_us);
    TEST_ASSERT_EQUAL_INT64(MATH_US_PER_MS, p.frame_us.min);
    TEST_ASSERT_EQUAL_INT64(3 * MATH_US_PER_MS, p.frame_us.max);
    TEST_ASSERT_EQUAL_INT64(2 * MATH_US_PER_MS, p.frame_us.avg);
    TEST_ASSERT_EQUAL_INT64(2 * MATH_US_PER_MS, p.frame_us.med);
    TEST_ASSERT_EQUAL_INT64(3 * MATH_US_PER_MS, p.frame_us.p95);
}

enum { SCRIPT_CAPACITY = 8, SAMPLE_US = FRAME_PROFILE_MIN_FRAME_US, CLIP_US = 3 * SAMPLE_US };

static frame_profile_t
play(fixture_t* f, int64_t duration) {
#ifdef HOST_HEAP_ARENA
    size_t blocks_before, bytes_before, blocks_after, bytes_after;
    heap_arena_snapshot(&blocks_before, &bytes_before);
#endif
    const frame_profile_scenario_t scenario = {"script", duration};
    const frame_profile_t p = frame_profile_play(&scenario, scripted_frame, f, NULL);
#ifdef HOST_HEAP_ARENA
    heap_arena_snapshot(&blocks_after, &bytes_after);
    TEST_ASSERT_EQUAL_size_t(blocks_before, blocks_after);
    TEST_ASSERT_EQUAL_size_t(bytes_before, bytes_after);
#endif
    return p;
}

static void
test_boundaries(void) {
    const int64_t warmups[] = {SAMPLE_US, SAMPLE_US - 1, 2 * SAMPLE_US, SAMPLE_US};
    const int frames[] = {3, 4, 2, SCRIPT_CAPACITY - 1};
    TEST_ASSERT_EQUAL_INT64(MATH_US_PER_MS, FRAME_PROFILE_MIN_FRAME_US);
    for (size_t c = 0; c < sizeof warmups / sizeof warmups[0]; c++) {
        int64_t times[SCRIPT_CAPACITY], steps[SCRIPT_CAPACITY];
        for (int i = 0; i < SCRIPT_CAPACITY; i++) {
            times[i] = SAMPLE_US;
            steps[i] = i == 0 ? 0 : i == 1 ? warmups[c] : SAMPLE_US;
        }
        times[0] = warmups[c];
        fixture_t f = {times, steps, SCRIPT_CAPACITY, 0};
        const int64_t duration = c == 3 ? (SCRIPT_CAPACITY - 1) * SAMPLE_US : CLIP_US;
        const frame_profile_t p = play(&f, duration);
        TEST_ASSERT_EQUAL_INT(frames[c], p.frames);
        TEST_ASSERT_EQUAL_INT(frames[c] + 1, f.calls);
        TEST_ASSERT_EQUAL_INT64(frames[c] * SAMPLE_US, p.total_us);
        TEST_ASSERT_EQUAL_INT64(SAMPLE_US, p.frame_us.avg);
    }
}

static void
test_invalid_frame_times(void) {
    const int64_t invalid[] = {0, -1, (int64_t)INT32_MAX + 1};
    const int indices[] = {0, 2};
    for (size_t c = 0; c < sizeof invalid / sizeof invalid[0]; c++) {
        for (size_t j = 0; j < sizeof indices / sizeof indices[0]; j++) {
            int64_t times[] = {SAMPLE_US, SAMPLE_US, SAMPLE_US};
            const int64_t steps[] = {0, SAMPLE_US, SAMPLE_US};
            times[indices[j]] = invalid[c];
            fixture_t f = {times, steps, sizeof times / sizeof times[0], 0};
            const frame_profile_t p = play(&f, CLIP_US);
            TEST_ASSERT_EQUAL_INT(indices[j] + 1, f.calls);
            TEST_ASSERT_EQUAL_INT(0, p.frames);
            TEST_ASSERT_EQUAL_INT64(0, p.total_us);
        }
    }
}

static void
test_invalid_duration(void) {
    const int64_t durations[] = {0, -1, (int64_t)INT_MAX * SAMPLE_US};
    for (size_t c = 0; c < sizeof durations / sizeof durations[0]; c++) {
        fixture_t f = {0};
        const frame_profile_t p = play(&f, durations[c]);
        TEST_ASSERT_EQUAL_INT(0, f.calls);
        TEST_ASSERT_EQUAL_INT(0, p.frames);
        TEST_ASSERT_EQUAL_INT64(0, p.total_us);
    }
}

static void
test_sample_capacity_fails_without_overrun(void) {
    const int64_t times[] = {1, 1, 1};
    const int64_t steps[] = {0, 1, 1};
    fixture_t f = {times, steps, sizeof times / sizeof times[0], 0};
    const frame_profile_t p = play(&f, SAMPLE_US);
    TEST_ASSERT_EQUAL_INT(0, p.frames);
    TEST_ASSERT_EQUAL_INT64(0, p.total_us);
    TEST_ASSERT_EQUAL_INT(f.count, f.calls);
}

#ifdef HOST_HEAP_ARENA
static void
test_allocation_failure(void) {
    void* held = memory_alloc(memory_largest_block(MEMORY_PSRAM), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(held);
    fixture_t f = {0};
    const frame_profile_t p = play(&f, CLIP_US);
    memory_free(held);
    TEST_ASSERT_EQUAL_INT(0, f.calls);
    TEST_ASSERT_EQUAL_INT(0, p.frames);
    TEST_ASSERT_EQUAL_INT64(0, p.total_us);
}
#endif

static void
test_trace_keeps_each_frame_at_its_path_time(void) {
    const int64_t times[] = {7 * MATH_US_PER_MS, MATH_US_PER_MS, 3 * MATH_US_PER_MS, 3 * MATH_US_PER_MS};
    const int64_t steps[] = {0, 7 * MATH_US_PER_MS, MATH_US_PER_MS, 3 * MATH_US_PER_MS};
    fixture_t f = {times, steps, sizeof times / sizeof times[0], 0};
    const frame_profile_scenario_t scenario = {"script", 10 * MATH_US_PER_MS};
    frame_profile_trace_t trace;
    const frame_profile_t p = frame_profile_play(&scenario, scripted_frame, &f, &trace);
    TEST_ASSERT_EQUAL_INT(p.frames, trace.frames);
    /* Each frame draws the path where the previous frames' times have carried it. */
    const int32_t path_ms[] = {7, 8, 11};
    for (int i = 0; i < trace.frames; i++) {
        TEST_ASSERT_EQUAL_INT32(path_ms[i], trace.frame[i].path_ms);
        TEST_ASSERT_EQUAL_INT32((int32_t)times[i + 1], trace.frame[i].frame_us);
    }
    frame_profile_trace_free(&trace);
    TEST_ASSERT_NULL(trace.frame);
    TEST_ASSERT_EQUAL_INT(0, trace.frames);
}

static void
test_trace_is_empty_when_the_profile_fails(void) {
    const int64_t times[] = {SAMPLE_US, SAMPLE_US, 0};
    const int64_t steps[] = {0, SAMPLE_US, SAMPLE_US};
    fixture_t f = {times, steps, sizeof times / sizeof times[0], 0};
    const frame_profile_scenario_t scenario = {"script", CLIP_US};
    frame_profile_trace_t trace = {(frame_profile_frame_t*)&f, 1};
    const frame_profile_t p = frame_profile_play(&scenario, scripted_frame, &f, &trace);
    TEST_ASSERT_EQUAL_INT(0, p.frames);
    TEST_ASSERT_NULL(trace.frame);
    TEST_ASSERT_EQUAL_INT(0, trace.frames);
}

static void
test_worst_frames_slowest_first(void) {
    frame_profile_frame_t frames[] = {{0, 4}, {1, 9}, {2, 1}, {3, 9}, {4, 7}, {5, 2}};
    const frame_profile_trace_t trace = {frames, sizeof frames / sizeof frames[0]};

    enum { K = 3 };

    int index[K + 1] = {-1, -1, -1, -1};
    TEST_ASSERT_EQUAL_INT(K, frame_profile_worst(&trace, K, index));
    const int expected[] = {1, 3, 4};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, index, K);
    TEST_ASSERT_EQUAL_INT(-1, index[K]);
    int all[sizeof frames / sizeof frames[0] + 1];
    const int count = (int)(sizeof frames / sizeof frames[0]);
    TEST_ASSERT_EQUAL_INT(count, frame_profile_worst(&trace, count + 1, all));
    const int ranked[] = {1, 3, 4, 0, 5, 2};
    TEST_ASSERT_EQUAL_INT_ARRAY(ranked, all, count);
    TEST_ASSERT_EQUAL_INT(0, frame_profile_worst(&trace, 0, index));
}

static void
test_budget_boundaries(void) {
    const int64_t ceiling = (int64_t)INT32_MAX + SAMPLE_US;
    TEST_ASSERT_FALSE(perf_goal("zero", INT64_MAX, 0));
    TEST_ASSERT_FALSE(perf_goal("equal", SAMPLE_US, SAMPLE_US));
    TEST_ASSERT_TRUE(perf_goal("above", SAMPLE_US + 1, SAMPLE_US));
    TEST_ASSERT_FALSE(perf_goal("below", SAMPLE_US - 1, SAMPLE_US));
    perf_guard("unpegged", INT64_MAX, 0);
    perf_guard("below", ceiling - 1, ceiling);
    TEST_ASSERT_TRUE(perf_guard_passes(INT64_MAX, 0));
    TEST_ASSERT_TRUE(perf_guard_passes(ceiling - 1, ceiling));
#if CONFIG_LAUNCHER_QEMU
    TEST_ASSERT_TRUE(perf_guard_passes(ceiling, ceiling));
#else
    TEST_ASSERT_FALSE(perf_guard_passes(ceiling, ceiling));
#endif
}

void
suite_frame_profile(void) {
    RUN_TEST(test_previous_frame_drives_duration_and_statistics);
    RUN_TEST(test_boundaries);
    RUN_TEST(test_invalid_frame_times);
    RUN_TEST(test_invalid_duration);
    RUN_TEST(test_sample_capacity_fails_without_overrun);
    RUN_TEST(test_trace_keeps_each_frame_at_its_path_time);
    RUN_TEST(test_trace_is_empty_when_the_profile_fails);
    RUN_TEST(test_worst_frames_slowest_first);
#ifdef HOST_HEAP_ARENA
    RUN_TEST(test_allocation_failure);
#endif
    RUN_TEST(test_budget_boundaries);
}

SUITE_REGISTER(suite_frame_profile);

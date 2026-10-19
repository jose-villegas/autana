#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "frame_profile.h"
#include "math/scalar/units.h"
#include "perf_budget.h"

enum { SCRIPT_CAPACITY = 8, SAMPLE_US = MATH_US_PER_MS, CLIP_US = 3 * SAMPLE_US };

static int allocations;
static int calls;
static int count;
static int64_t* times;
static int64_t previous;
static bool fail_allocation;

void*
heap_caps_malloc(size_t bytes, uint32_t caps) {
    (void)caps;
    if (fail_allocation) {
        return NULL;
    }
    void* block = malloc(bytes);
    allocations += block != NULL;
    return block;
}

void
heap_caps_free(void* block) {
    allocations -= block != NULL;
    free(block);
}

void
setUp(void) {}

void
tearDown(void) {}

static int64_t
frame(void* ctx, int64_t dt_us) {
    (void)ctx;
    assert(calls < count);
    assert(dt_us == previous);
    previous = times[calls++];
    return previous;
}

static void
guard_case(int64_t measured, int64_t ceiling, bool fails) {
    UnityBegin("guard");
    Unity.CurrentTestName = "guard";
    if (TEST_PROTECT()) {
        perf_guard("device_tests", "probe", measured, ceiling);
    }
    assert((Unity.CurrentTestFailed != 0) == fails);
}

int
main(int argc, char** argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "budget") == 0) {
        const int64_t ceiling = (int64_t)INT32_MAX + SAMPLE_US;
        assert(!perf_goal("device_tests", "zero", INT64_MAX, 0));
        assert(!perf_goal("device_tests", "equal", SAMPLE_US, SAMPLE_US));
        assert(perf_goal("device_tests", "above", SAMPLE_US + 1, SAMPLE_US));
        assert(!perf_goal("device_tests", "below", SAMPLE_US - 1, SAMPLE_US));
        assert(!perf_goal("capybara_perf", "skin 2 core(s)", SAMPLE_US, SAMPLE_US));
        guard_case(INT64_MAX, 0, false);
        guard_case(ceiling - 1, ceiling, false);
        guard_case(ceiling, ceiling, !CONFIG_LAUNCHER_QEMU);
        return 0;
    }
    int64_t script[SCRIPT_CAPACITY];
    times = script;
    int64_t duration = CLIP_US;
    count = SCRIPT_CAPACITY;
    for (int i = 0; i < count; i++) {
        times[i] = SAMPLE_US;
    }
    bool failed = false;
    int frames = CLIP_US / SAMPLE_US;
    if (strcmp(argv[1], "floor") == 0) {
        assert(FRAME_PROFILE_MIN_FRAME_US == MATH_US_PER_MS);
    } else if (strcmp(argv[1], "maximum") == 0) {
        duration = (SCRIPT_CAPACITY - 1) * FRAME_PROFILE_MIN_FRAME_US;
        frames = SCRIPT_CAPACITY - 1;
    } else if (strcmp(argv[1], "short") == 0) {
        times[0]--;
        frames++;
    } else if (strcmp(argv[1], "warm") == 0) {
        times[0] = SAMPLE_US * 2;
        frames--;
    } else if (strcmp(argv[1], "zero") == 0 || strcmp(argv[1], "negative") == 0) {
        duration = strcmp(argv[1], "zero") == 0 ? 0 : -1;
        failed = true;
    } else if (strcmp(argv[1], "allocation") == 0) {
        fail_allocation = true;
        failed = true;
    } else if (strcmp(argv[1], "duration_overflow") == 0) {
        duration = (int64_t)INT_MAX * FRAME_PROFILE_MIN_FRAME_US;
        failed = true;
    } else if (strcmp(argv[1], "overflow") == 0) {
        duration = FRAME_PROFILE_MIN_FRAME_US;
        for (int i = 0; i < count; i++) {
            times[i] = 1;
        }
        failed = true;
    } else if (strncmp(argv[1], "bad", 3) == 0) {
        const int index = argv[1][3] == 'w' ? 0 : 2;
        times[index] = argv[1][4] == '0' ? 0 : argv[1][4] == '-' ? -1 : (int64_t)INT32_MAX + 1;
        failed = true;
    }
    const frame_profile_scenario_t scenario = {"probe", duration};
    const frame_profile_t p = frame_profile_play(&scenario, frame, NULL);
    assert(allocations == 0);
    assert(p.frames == (failed ? 0 : frames));
    assert(p.total_us == (failed ? 0 : frames * SAMPLE_US));
    if (duration <= 0 || fail_allocation || strcmp(argv[1], "duration_overflow") == 0) {
        assert(calls == 0);
    }
    if (!failed) {
        assert(calls == frames + 1);
        assert(p.frame_us.avg == SAMPLE_US);
    }
    frame_profile_log("probe", scenario.row, &p, "portrait");
    return 0;
}

#include "suites.h"
#include "unity.h"

#include "apps/sand/sand_controls.h"
#include "apps/sand/tests/motion_replay_generated.h"
#include "input/motion/gravity.h"
#include "input/motion/shake.h"
#ifdef DEVICE_BUILD
#include "core/memory.h"
#include "core/timing.h"
#include "esp_cpu.h"
#include "perf_stats.h"
#include "profile/frame_cost.h"
#include "services/build_id.h"
#include "test_cleanup.h"
#endif

#define MOTION_ROTATION_X_DPS 30
#define MOTION_ROTATION_Y_DPS (-20)
#define MOTION_ROTATION_Z_DPS 10
#define MOTION_PERCENT        100.0
#define MOTION_FRAME_COUNT    (sizeof motion_replay_acceleration / sizeof *motion_replay_acceleration)
#define MOTION_DT_US          (SAND_STEP_MS * MATH_US_PER_MS)

_Static_assert(SAND_STEP_MS == MOTION_REPLAY_FRAME_MS, "regenerate replay for the current frame interval");

typedef struct {
    gravity_t gravity;
    shake_t shake;
    input_motion_t motion;
    vec3f_t linear;
    int jostle;
} frame_t;

typedef void (*step_t)(frame_t*);

static __attribute__((noinline)) void
whole_step(frame_t* f) {
    gravity_update(&f->gravity, &f->motion, MOTION_DT_US);
    shake_update(&f->shake, gravity_linear(&f->gravity), MOTION_DT_US);
    f->jostle = sand_controls_jostle(shake_level(&f->shake));
}

static void
test_recorded_motion_replay(void) {
    frame_t f = {0};
    gravity_reset(&f.gravity);
    shake_reset(&f.shake);
    int peak = 0;
    for (size_t i = 0; i < MOTION_FRAME_COUNT; i++) {
        f.motion = (input_motion_t){.acceleration = motion_replay_acceleration[i]};
        whole_step(&f);
        if (f.jostle > peak) {
            peak = f.jostle;
        }
    }
    TEST_ASSERT_GREATER_THAN_INT(0, peak);
    TEST_ASSERT_EQUAL_INT(0, f.jostle);
}

#ifdef DEVICE_BUILD
static __attribute__((noinline)) void
gravity_step(frame_t* f) {
    gravity_update(&f->gravity, &f->motion, MOTION_DT_US);
}

static __attribute__((noinline)) void
shake_step(frame_t* f) {
    shake_update(&f->shake, f->linear, MOTION_DT_US);
    f->jostle = sand_controls_jostle(shake_level(&f->shake));
}

static const struct {
    const char* name;
    step_t step;
} rows[] = {{"gravity", gravity_step}, {"shake_jostle", shake_step}, {"whole", whole_step}};

static input_motion_t* motion;

static void
release(void) {
    memory_free(motion);
    motion = NULL;
}

static void
fixture(bool rotating) {
    motion = memory_alloc(MOTION_FRAME_COUNT * sizeof *motion, MEMORY_INTERNAL);
    suite_set_test_cleanup(release);
    TEST_ASSERT_NOT_NULL(motion);
    const vec3i_t rotation = rotating ? (vec3i_t){MOTION_ROTATION_X_DPS * INPUT_ROTATION_ONE_DPS,
                                                  MOTION_ROTATION_Y_DPS * INPUT_ROTATION_ONE_DPS,
                                                  MOTION_ROTATION_Z_DPS * INPUT_ROTATION_ONE_DPS}
                                      : (vec3i_t){0};
    for (size_t i = 0; i < MOTION_FRAME_COUNT; i++) {
        motion[i] = (input_motion_t){.acceleration = motion_replay_acceleration[i], .rotation = rotation};
    }
}

static float
median_cycles(step_t step, const frame_t* before, bool cold) {
    _Static_assert(PERF_CYCLE_COLD_REPS <= PERF_CYCLE_WARM_REPS, "cold samples must fit warm storage");
    int32_t samples[PERF_CYCLE_WARM_REPS];
    const int reps = cold ? PERF_CYCLE_COLD_REPS : PERF_CYCLE_WARM_REPS;
    frame_t trial = *before;
    step(&trial);
    for (int rep = 0; rep < reps; rep++) {
        trial = *before;
        if (cold) {
            perf_cache_cold();
        }
        const uint32_t start = esp_cpu_get_cycle_count();
        step(&trial);
        samples[rep] = (int32_t)(esp_cpu_get_cycle_count() - start);
        __asm__ volatile("" : : "r"(&trial) : "memory");
    }
    return (float)perf_stats_compute(samples, reps).med;
}

static void
report_rows(bool rotating) {
    fixture(rotating);
    for (size_t row = 0; row < sizeof rows / sizeof *rows; row++) {
        frame_t f = {0};
        gravity_reset(&f.gravity);
        shake_reset(&f.shake);
        double warm = 0, cold = 0;
        for (size_t i = 0; i < MOTION_FRAME_COUNT; i++) {
            f.motion = motion[i];
            frame_t next = f;
            whole_step(&next);
            f.linear = gravity_linear(&next.gravity);
            warm += median_cycles(rows[row].step, &f, false);
            cold += median_cycles(rows[row].step, &f, true);
            f = next;
            timing_yield();
        }
        warm /= MOTION_FRAME_COUNT;
        cold /= MOTION_FRAME_COUNT;
        const double warm_us = warm / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        const double cold_us = cold / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        printf("MOTIONBENCH name=%s rotation=%s frames=%u dt_us=%u warm_cycles=%.1f cold_cycles=%.1f "
               "warm_us=%.3f cold_us=%.3f warm_frame_pct=%.5f cold_frame_pct=%.5f\n",
               rows[row].name, rotating ? "typical" : "zero", (unsigned)MOTION_FRAME_COUNT, (unsigned)MOTION_DT_US,
               warm, cold, warm_us, cold_us, warm_us * SAND_STEP_HZ * MOTION_PERCENT / MATH_US_PER_S,
               cold_us * SAND_STEP_HZ * MOTION_PERCENT / MATH_US_PER_S);
    }
    release();
    suite_clear_test_cleanup();
}

static void
test_motion_board_rows(void) {
#if CONFIG_LAUNCHER_QEMU
    TEST_IGNORE_MESSAGE("QEMU has no hardware cache timing");
#endif
    TEST_ASSERT_EQUAL_INT(0, esp_cpu_get_core_id());
    TEST_ASSERT_TRUE(frame_cost_counters_idle());
    printf("BUILD_ID=%s\n", build_id());
    report_rows(false);
    report_rows(true);
}
#endif

void
run_sand_motion_perf_suite(void) {
    RUN_TEST(test_recorded_motion_replay);
#ifdef DEVICE_BUILD
    RUN_TEST(test_motion_board_rows);
#endif
}

SUITE_REGISTER_ON_REQUEST(run_sand_motion_perf_suite);

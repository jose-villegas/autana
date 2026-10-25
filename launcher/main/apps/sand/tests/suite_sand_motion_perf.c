#include "suites.h"
#include "unity.h"

#include <math.h>

#include "apps/sand/sand.h"
#include "apps/sand/sand_controls.h"
#include "input/motion/gravity.h"
#include "input/motion/shake.h"
#include "math/scalar/math_const.h"
#ifdef DEVICE_BUILD
#include "core/memory.h"
#include "core/timing.h"
#include "esp_cpu.h"
#include "profile/frame_cost.h"
#include "services/build_id.h"
#include "testkit/bench.h"
#include "testkit/cleanup.h"
#endif

#define MOTION_ROTATION_X_DPS 30
#define MOTION_ROTATION_Y_DPS (-20)
#define MOTION_ROTATION_Z_DPS 10
#define MOTION_PERCENT        100.0f
#define MOTION_STILL_FRAMES   100
#define MOTION_HARD_FRAMES    100
#define MOTION_SETTLE_FRAMES  60
#define MOTION_SHAKE_HZ       4.0F
#define MOTION_SHAKE_G        3.0F
#define MOTION_ACTIVE_G       0.75F
#define MOTION_FRAME_COUNT    (MOTION_STILL_FRAMES + MOTION_HARD_FRAMES + MOTION_STILL_FRAMES)
#define MOTION_DT_US          (SAND_STEP_MS * MATH_US_PER_MS)

static input_motion_t
replay_frame(size_t i, bool rotating) {
    float linear_g = 0;
    if (i >= MOTION_STILL_FRAMES && i < MOTION_STILL_FRAMES + MOTION_HARD_FRAMES) {
        const float seconds = (float)(i - MOTION_STILL_FRAMES) * SAND_STEP_MS / MATH_MS_PER_S;
        linear_g = MOTION_SHAKE_G * sinf(MATH_TAU * MOTION_SHAKE_HZ * seconds);
    }
    return (input_motion_t){.acceleration = {(int)lroundf(linear_g * INPUT_ACCEL_ONE_G), INPUT_ACCEL_ONE_G, 0},
                            .rotation = rotating ? (vec3i_t){MOTION_ROTATION_X_DPS * INPUT_ROTATION_ONE_DPS,
                                                             MOTION_ROTATION_Y_DPS * INPUT_ROTATION_ONE_DPS,
                                                             MOTION_ROTATION_Z_DPS * INPUT_ROTATION_ONE_DPS}
                                                 : (vec3i_t){0}};
}

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
test_motion_replay(void) {
    /* A fixed one-g vector with nonzero gyro is not physical motion. */
    frame_t f = {0};
    gravity_reset(&f.gravity);
    shake_reset(&f.shake);
    int peak = 0;
    bool onset = false;
    for (size_t i = 0; i < MOTION_FRAME_COUNT; i++) {
        f.motion = replay_frame(i, false);
        whole_step(&f);
        if (i < MOTION_STILL_FRAMES || i >= MOTION_STILL_FRAMES + MOTION_HARD_FRAMES + MOTION_SETTLE_FRAMES) {
            TEST_ASSERT_EQUAL_INT(0, f.jostle);
        }
        if (!onset && f.shake.smooth_g >= MOTION_ACTIVE_G) {
            TEST_ASSERT_GREATER_THAN_INT(0, f.jostle);
            onset = true;
        }
        if (i >= MOTION_STILL_FRAMES && i < MOTION_STILL_FRAMES + MOTION_HARD_FRAMES && f.jostle > peak) {
            peak = f.jostle;
        }
    }
    TEST_ASSERT_TRUE(onset);
    TEST_ASSERT_EQUAL_INT(SAND_JOSTLE_MAX, peak);
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
static frame_t* trials;

static void
release(void) {
    memory_free(trials);
    trials = NULL;
    memory_free(motion);
    motion = NULL;
}

static void
fixture(bool rotating) {
    motion = memory_alloc(MOTION_FRAME_COUNT * sizeof *motion, MEMORY_INTERNAL);
    trials = memory_alloc((BENCH_CYCLE_WARM_REPS + 1) * sizeof *trials, MEMORY_INTERNAL);
    suite_set_test_cleanup(release);
    TEST_ASSERT_NOT_NULL(motion);
    TEST_ASSERT_NOT_NULL(trials);
    for (size_t i = 0; i < MOTION_FRAME_COUNT; i++) {
        motion[i] = replay_frame(i, rotating);
    }
}

typedef struct {
    step_t step;
    frame_t* trial;
} trial_t;

static void
run_trial(void* ctx) {
    trial_t* t = ctx;
    t->step(t->trial++);
}

static float
measure_step(step_t step, const frame_t* before, bool cold) {
    for (size_t i = 0; i < BENCH_CYCLE_WARM_REPS + 1; i++) {
        trials[i] = *before;
    }
    trial_t ctx = {.step = step, .trial = trials};
    return bench_cycle_median(run_trial, &ctx, cold);
}

static void
report_rows(bool rotating) {
    fixture(rotating);
    for (size_t row = 0; row < sizeof rows / sizeof *rows; row++) {
        frame_t f = {0};
        gravity_reset(&f.gravity);
        shake_reset(&f.shake);
        float warm = 0;
        float cold = 0;
        for (size_t i = 0; i < MOTION_FRAME_COUNT; i++) {
            f.motion = motion[i];
            frame_t next = f;
            whole_step(&next);
            f.linear = gravity_linear(&next.gravity);
            warm += measure_step(rows[row].step, &f, false);
            cold += measure_step(rows[row].step, &f, true);
            f = next;
            timing_yield();
        }
        warm /= MOTION_FRAME_COUNT;
        cold /= MOTION_FRAME_COUNT;
        const float warm_us = warm / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        const float cold_us = cold / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        printf("MOTIONBENCH name=%s rotation=%s frames=%u dt_us=%u warm_cycles=%.1f cold_cycles=%.1f "
               "warm_us=%.3f cold_us=%.3f warm_frame_pct=%.5f cold_frame_pct=%.5f\n",
               rows[row].name, rotating ? "typical" : "zero", (unsigned)MOTION_FRAME_COUNT, (unsigned)MOTION_DT_US,
               (double)warm, (double)cold, (double)warm_us, (double)cold_us,
               (double)(warm_us * SAND_STEP_HZ * MOTION_PERCENT / MATH_US_PER_S),
               (double)(cold_us * SAND_STEP_HZ * MOTION_PERCENT / MATH_US_PER_S));
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
    RUN_TEST(test_motion_replay);
#ifdef DEVICE_BUILD
    RUN_TEST(test_motion_board_rows);
#endif
}

SUITE_REGISTER_ON_REQUEST(run_sand_motion_perf_suite);

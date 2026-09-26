/* Device-only suite: deterministic launcher-backdrop performance. */
#include "suites.h"

#ifdef DEVICE_BUILD

#include <stdint.h>

#include "unity.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "gfx/gfx.h"
#include "ui/ui_ridge.h"

static const char* TAG = "ridge_perf";

#define ARM_MS             10000
#define FRAME_DT_MS        16
#define LAUNCHER_HEAL_ROWS 32

typedef enum {
    ARM_IDLE,
    ARM_PLUCK_STRUM,
    ARM_TILT_SWEEP,
} arm_t;

typedef struct {
    int64_t elapsed_us;
    int64_t step_us;
    int64_t present_us;
    int64_t bytes;
    int frames;
} arm_result_t;

static const input_t idle_input = {0};

static input_t
pluck_strum_input(int frame) {
    return (input_t){
        .x = (frame * 19) % GFX_WIDTH,
        .y = GFX_HEIGHT / 2,
        .down = true,
        .pressed = frame == 0,
    };
}

static void
set_sweep_gravity(int frame) {
    static const int gravity[][2] = {
        {-256, 0}, {-192, -128}, {-128, -192}, {0, -256}, {128, -192}, {192, -128},
        {256, 0},  {192, 128},   {128, 192},   {0, 256},  {-128, 192}, {-192, 128},
    };
    const int phase = (frame / 12) % (int)(sizeof gravity / sizeof gravity[0]);
    ui_ridge_set_gravity(gravity[phase][0], gravity[phase][1], 256, 0);
}

static void
prime(void) {
    ui_ridge_reset_for_test();
    gfx_heal_set_budget(GFX_WIDTH * LAUNCHER_HEAL_ROWS);
    gfx_heal_set_rolling(LAUNCHER_HEAL_ROWS);
    ui_ridge_step(&idle_input, FRAME_DT_MS);
    gfx_present();
}

static arm_result_t
run_arm(arm_t arm) {
    arm_result_t result = {0};
    prime();
    gfx_reset_strip_send_counts();

    const int64_t began = esp_timer_get_time();
    while (esp_timer_get_time() - began < (int64_t)ARM_MS * 1000) {
        input_t input = idle_input;
        if (arm == ARM_PLUCK_STRUM) {
            input = pluck_strum_input(result.frames);
        } else if (arm == ARM_TILT_SWEEP) {
            set_sweep_gravity(result.frames);
        }

        int64_t phase = esp_timer_get_time();
        ui_ridge_step(&input, FRAME_DT_MS);
        result.step_us += esp_timer_get_time() - phase;

        phase = esp_timer_get_time();
        gfx_present();
        result.present_us += esp_timer_get_time() - phase;
        result.frames++;
    }
    result.elapsed_us = esp_timer_get_time() - began;
    result.bytes = gfx_get_bytes_sent();
    return result;
}

static const char*
arm_name(arm_t arm) {
    switch (arm) {
        case ARM_IDLE: return "idle";
        case ARM_PLUCK_STRUM: return "pluck_strum";
        case ARM_TILT_SWEEP: return "tilt_sweep";
    }
    return "unknown";
}

static void
log_arm(arm_t arm, const arm_result_t* result) {
    const double frames = result->frames;
    ESP_LOGI(TAG,
             "RIDGE PERF arm=%s frames=%d total_ms=%.3f step_ms=%.3f present_ms=%.3f fps=%.1f bytes_per_frame=%.0f",
             arm_name(arm), result->frames, (double)result->elapsed_us / frames / 1000.0,
             (double)result->step_us / frames / 1000.0, (double)result->present_us / frames / 1000.0,
             1000000.0 * frames / result->elapsed_us, (double)result->bytes / frames);
}

static void
assert_arm(const arm_result_t* result) {
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, result->frames, "no frames captured");
    TEST_ASSERT_TRUE_MESSAGE(result->step_us > 0, "ridge step did no work");
    TEST_ASSERT_TRUE_MESSAGE(result->present_us > 0, "present did no work");
    TEST_ASSERT_TRUE_MESSAGE(result->bytes > 0, "no panel bytes sent");
}

void
test_ridge_performance(void) {
    for (arm_t arm = ARM_IDLE; arm <= ARM_TILT_SWEEP; arm++) {
        const arm_result_t result = run_arm(arm);
        log_arm(arm, &result);
        assert_arm(&result);
    }
    gfx_heal_restore_defaults();
    TEST_PASS();
}

void
run_ridge_perf_suite(void) {
    RUN_TEST(test_ridge_performance);
}

#else

void
run_ridge_perf_suite(void) {}

#endif

SUITE_REGISTER(run_ridge_perf_suite);

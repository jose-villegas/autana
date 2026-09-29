/* Device-only suite: deterministic launcher-backdrop performance. */
#include "suites.h"

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "gfx/gfx.h"
#include "ridge_arms.h"
#include "ui/ui_ridge.h"
#include "util/tune.h"

static const char* TAG = "ridge_perf";

#define ARM_MS             10000
#define FRAME_DT_MS        RIDGE_ARM_FRAME_DT_MS
#define LAUNCHER_HEAL_ROWS 32

typedef struct {
    int64_t elapsed_us;
    int64_t step_us;
    int64_t present_us;
    int64_t bytes;
    int64_t heal_bytes;
    int frames, full_bands, gathered, partial_bands;
} arm_result_t;

static void
prime(void) {
    ui_ridge_reset_for_test();
    gfx_heal_set_budget(GFX_WIDTH * LAUNCHER_HEAL_ROWS);
    gfx_heal_set_rolling(LAUNCHER_HEAL_ROWS);
    ui_ridge_step(&ridge_arm_idle_input, FRAME_DT_MS);
    gfx_present();
}

static arm_result_t
run_arm(arm_t arm) {
    arm_result_t result = {0};
    prime();
    ui_ridge_set_ambient(arm == ARM_AMBIENT || arm == ARM_AMBIENT_PORTRAIT);
    if (arm == ARM_AMBIENT_PORTRAIT) {
        ui_ridge_set_gravity(0, 256, 256, 0);
        for (int frame = 0; frame < 200; frame++) {
            ui_ridge_step(&ridge_arm_idle_input, FRAME_DT_MS);
            gfx_present();
        }
        gfx_reset_strip_send_counts();
    }
    gfx_reset_strip_send_counts();

    const int64_t began = esp_timer_get_time();
    while (esp_timer_get_time() - began < (int64_t)ARM_MS * 1000) {
        input_t input;
        ridge_arm_drive(arm, result.frames, &input);

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
    result.heal_bytes = gfx_get_heal_bytes_sent();
    gfx_get_strip_send_counts(&result.full_bands, &result.gathered, &result.partial_bands);
    return result;
}

static const char*
arm_name(arm_t arm) {
    switch (arm) {
        case ARM_IDLE: return "idle";
        case ARM_PLUCK_STRUM: return "pluck_strum";
        case ARM_TILT_SWEEP: return "tilt_sweep";
        case ARM_TILT_WOBBLE: return "tilt_wobble";
        case ARM_AMBIENT: return "ambient";
        case ARM_AMBIENT_PORTRAIT: return "ambient_portrait";
    }
    return "unknown";
}

static void
log_arm(arm_t arm, const arm_result_t* result) {
    const double frames = result->frames;
    ESP_LOGI(TAG,
             "RIDGE PERF arm=%s frames=%d total_ms=%.3f step_ms=%.3f present_ms=%.3f fps=%.1f bytes_per_frame=%.0f "
             "heal_bytes_per_frame=%.0f full_bands=%d gathered=%d partial_bands=%d",
             arm_name(arm), result->frames, (double)result->elapsed_us / frames / 1000.0,
             (double)result->step_us / frames / 1000.0, (double)result->present_us / frames / 1000.0,
             1000000.0 * frames / result->elapsed_us, (double)result->bytes / frames,
             (double)result->heal_bytes / frames, result->full_bands, result->gathered, result->partial_bands);
}

static void
assert_arm(const arm_result_t* result) {
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, result->frames, "no frames captured");
    TEST_ASSERT_TRUE_MESSAGE(result->step_us > 0, "ridge step did no work");
    TEST_ASSERT_TRUE_MESSAGE(result->present_us > 0, "present did no work");
    TEST_ASSERT_TRUE_MESSAGE(result->bytes > 0, "no panel bytes sent");
}

typedef struct {
    int count, x0, x1, y0, y1;
} unlike_t;

static void
note_unlike(unlike_t* unlike, size_t at, gfx_color_t repainted, gfx_color_t full) {
    const int x = (int)(at % GFX_WIDTH), y = (int)(at / GFX_WIDTH);
    unlike->x0 = x < unlike->x0 ? x : unlike->x0;
    unlike->x1 = x > unlike->x1 ? x : unlike->x1;
    unlike->y0 = y < unlike->y0 ? y : unlike->y0;
    unlike->y1 = y > unlike->y1 ? y : unlike->y1;
    if (unlike->count < 4) {
        ESP_LOGI(TAG, "unlike x=%d y=%d repainted=%04x full=%04x", x, y, repainted, full);
    }
    unlike->count++;
}

static int
unlike_a_full_paint_now(void) {
    const size_t bytes = (size_t)GFX_WIDTH * GFX_HEIGHT * sizeof(gfx_color_t);
    gfx_color_t* const repainted = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    TEST_ASSERT_NOT_NULL(repainted);
    memcpy(repainted, gfx_framebuffer(), bytes);
    ui_ridge_paint();
    unlike_t unlike = {0, GFX_WIDTH, -1, GFX_HEIGHT, -1};
    for (size_t i = 0; i < (size_t)GFX_WIDTH * GFX_HEIGHT; i++) {
        if (repainted[i] != gfx_framebuffer()[i]) {
            note_unlike(&unlike, i, repainted[i], gfx_framebuffer()[i]);
        }
    }
    if (unlike.count) {
        ESP_LOGI(TAG, "unlike box x %d..%d y %d..%d", unlike.x0, unlike.x1, unlike.y0, unlike.y1);
    }
    heap_caps_free(repainted);
    return unlike.count;
}

/* The frame the incremental repaint leaves must be the one a full paint
 * draws from the same state, and with the send audit on, every pixel it
 * changed must have reached the panel: a forgotten repaint or an unmarked
 * change shows here. `dissolved`, when given, counts the frames a strip
 * switch spent dissolving in. */
static int
pixels_unlike_a_full_paint_counting(arm_t arm, int frames, int* dissolved) {
    prime();
    gfx_set_send_audit(true);
    TEST_ASSERT_TRUE_MESSAGE(gfx_send_audit(), "the send audit did not come on");
    for (int frame = 0; frame < frames; frame++) {
        input_t input;
        ridge_arm_drive(arm, frame, &input);
        ui_ridge_step(&input, FRAME_DT_MS);
        gfx_present();
        if (dissolved != NULL && ui_ridge_dissolving_for_test()) {
            (*dissolved)++;
        }
    }
    /* A switch between strips still dissolving in is by design unlike a
     * full paint: let it finish, holding the last tilt. */
    for (int frame = 0; frame < 240 && ui_ridge_dissolving_for_test(); frame++) {
        ui_ridge_step(&ridge_arm_idle_input, FRAME_DT_MS);
        gfx_present();
    }
    TEST_ASSERT_FALSE_MESSAGE(ui_ridge_dissolving_for_test(), "a strip switch never finished dissolving in");
    const int64_t unsent = gfx_send_audit_uncovered_px();
    gfx_set_send_audit(false);
    TEST_ASSERT_TRUE_MESSAGE(unsent == 0, "a repainted pixel never reached the panel");
    return unlike_a_full_paint_now();
}

static int
pixels_unlike_a_full_paint(arm_t arm, int frames) {
    return pixels_unlike_a_full_paint_counting(arm, frames, NULL);
}

static void
ignore_tune_reply(const char* line) {
    (void)line;
}

/* The dissolve a development build can turn on for the strip switch: it
 * must run through a sweep and still end on a full paint's frame, every
 * changed pixel sent. */
void
test_ridge_dissolve_matches_a_full_paint(void) {
    tune_handle_line("SET ridge.axis_dissolve_ms 400", ignore_tune_reply);
    int dissolved = 0;
    const int unlike = pixels_unlike_a_full_paint_counting(ARM_TILT_SWEEP, 150, &dissolved);
    tune_handle_line("RESET ridge.axis_dissolve_ms", ignore_tune_reply);
    gfx_heal_restore_defaults();
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, dissolved, "the sweep never dissolved a strip switch");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, unlike, "after a dissolved strip switch");
}

/* After the tilting stops and the board is held about 20 degrees off level,
 * the ridge must come to rest on what it shows: nothing left moving, and the
 * frame on screen the one a full paint draws. */
void
test_ridge_settles_after_tilting(void) {
    prime();
    input_t input;
    for (int frame = 0; frame < 160; frame++) {
        ridge_arm_drive(ARM_TILT_WOBBLE, frame, &input);
        ui_ridge_step(&input, FRAME_DT_MS);
        gfx_present();
    }
    ui_ridge_set_gravity(-240, 88, 256, 0);
    for (int frame = 0; frame < 150; frame++) {
        ui_ridge_step(&ridge_arm_idle_input, FRAME_DT_MS);
        gfx_present();
    }
    /* A turn of about a degree, less than the gradient follows in one step:
     * it catches up only once the ridge holds still. */
    ui_ridge_set_gravity(-241, 84, 256, 0);
    for (int frame = 0; frame < 150; frame++) {
        ui_ridge_step(&ridge_arm_idle_input, FRAME_DT_MS);
        gfx_present();
    }
    gfx_reset_strip_send_counts();
    for (int frame = 0; frame < 60; frame++) {
        ui_ridge_step(&ridge_arm_idle_input, FRAME_DT_MS);
        gfx_present();
    }
    const int64_t per_frame = gfx_get_bytes_sent() / 60;
    const int unlike = unlike_a_full_paint_now();
    gfx_heal_restore_defaults();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, unlike, "the held frame is not what a full paint draws");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ui_ridge_gradient_lag_for_test(), "the gradient never caught up with the ridge");
    ESP_LOGI(TAG, "RIDGE SETTLE bytes_per_frame=%lld", (long long)per_frame);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(GFX_WIDTH * LAUNCHER_HEAL_ROWS * 2, (int)per_frame,
                                          "still repainting after the tilt stopped");
}

void
test_ridge_repaint_matches_a_full_paint(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pixels_unlike_a_full_paint(ARM_PLUCK_STRUM, 90), "after a pluck and strum");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pixels_unlike_a_full_paint(ARM_TILT_SWEEP, 150), "through a tilt sweep");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pixels_unlike_a_full_paint(ARM_TILT_WOBBLE, 150), "tilting back and forth");
    gfx_heal_restore_defaults();
}

void
test_ridge_performance(void) {
    for (arm_t arm = ARM_IDLE; arm <= ARM_AMBIENT_PORTRAIT; arm++) {
        const arm_result_t result = run_arm(arm);
        log_arm(arm, &result);
        assert_arm(&result);
    }
    gfx_heal_restore_defaults();
    TEST_PASS();
}

void
run_ridge_perf_suite(void) {
    RUN_TEST(test_ridge_repaint_matches_a_full_paint);
    RUN_TEST(test_ridge_dissolve_matches_a_full_paint);
    RUN_TEST(test_ridge_settles_after_tilting);
    RUN_TEST(test_ridge_performance);
}

#else

void
run_ridge_perf_suite(void) {}

#endif

SUITE_REGISTER(run_ridge_perf_suite);

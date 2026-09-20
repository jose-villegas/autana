/*
 * Device-only suite: the Cornell tracer's panel SEND cost. suite_rt_perf.c
 * isolates the orientation-neutral tracer arithmetic by tracing with no
 * present; this drives the same resolve into the real framebuffer, HUD
 * included, and times gfx_present() itself - its own file, since folding
 * a present into suite_rt_perf.c would falsify what that header claims.
 *
 * Runs under DEVICE_BUILD only - needs a real panel, DMA and a clock.
 */
#include "suites.h" /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "unity.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "display/display.h"
#include "gfx/gfx.h"
#include "render_lab.h"
#include "rt_cornell.h"
#include "rt_pixel_budget.h"
#include "rt_refine.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

/* app_render_lab.c's own HUD entry point - suite_cube_perf.c's precedent
 * for reaching it from a suite outside the app's normal frame loop. */
extern void draw_fps(const input_t* input, bool for_bands);

static const char* TAG = "rt_present_perf";

/* Mirrors scene_raytrace.c's own tuning, so the budgeted chunk size and its
 * frame-to-frame adaptation match what the board actually runs. */
#define TARGET_FRAME_MS     30
#define PIXEL_BUDGET_MIN    64
#define PIXEL_BUDGET_GROWTH GFX_WIDTH
#define PIXEL_BUDGET_MAX    (GFX_WIDTH * GFX_HEIGHT)

static const input_t null_input = {0};

/* One refinement pass's totals, summed across every budgeted chunk (every
 * present) that pass took to resolve. trace_us/present_us are clocks
 * (esp_timer_get_time()); frames/transfers/bytes are counters. */
typedef struct {
    int64_t trace_us;
    int64_t present_us;
    int64_t bytes;
    int transfers;
    int frames;
} pass_totals_t;

static void
log_pass(int quarter, bool hud_on, bool dual_core, int pass, const pass_totals_t* t) {
    ESP_LOGI(TAG,
             "quarter=%d hud=%-3s cores=%-6s pass %d/%d frames=%-4d trace=%8lldus(%.1f/f) present=%8lldus(%.1f/f) "
             "transfers=%-4d(%.2f/f) bytes=%lld(%.0f/f)",
             quarter, hud_on ? "on" : "off", dual_core ? "dual" : "single", pass + 1, RT_REFINE_PASSES, t->frames,
             (long long)t->trace_us, (double)t->trace_us / t->frames, (long long)t->present_us,
             (double)t->present_us / t->frames, t->transfers, (double)t->transfers / t->frames, (long long)t->bytes,
             (double)t->bytes / t->frames);
}

/* Every budgeted chunk of every pass - scene_raytrace_frame()'s own state
 * machine unrolled into one loop instead of one shell frame() per chunk.
 * dual_core selects rt_cornell_render_rows() (no job dispatch) against
 * rt_cornell_render_lattice_budget() (the real core-1 split). */
static void
run_arm(int quarter, bool hud_on, bool dual_core, pass_totals_t out[RT_REFINE_PASSES]) {
    for (int i = 0; i < RT_REFINE_PASSES; i++) {
        out[i] = (pass_totals_t){0};
    }

    rt_cornell_camera_t cam;
    rt_cornell_camera_init(&cam, (r3d_viewport_t){GFX_WIDTH, GFX_HEIGHT, quarter});
    gfx_color_t* fb = gfx_framebuffer();

    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));

    gfx_set_partial_clear(false);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));
    gfx_invalidate();
    gfx_present(); /* untimed: flushes the clear so pass 1's own present is not padded by it */

    rt_pixel_budget_t budget =
        rt_pixel_budget_init(PIXEL_BUDGET_GROWTH, PIXEL_BUDGET_MIN, PIXEL_BUDGET_MAX, TARGET_FRAME_MS);
    uint32_t dt_ms = 0;
    int step = RT_REFINE_FIRST_STEP;
    int next_y = 0;

    for (int pass = 0; pass < RT_REFINE_PASSES; pass++) {
        while (next_y < GFX_HEIGHT) {
            rt_pixel_budget_adapt(&budget, dt_ms);

            const int first_y = next_y;
            const int64_t trace_start = esp_timer_get_time();
            if (dual_core) {
                next_y = rt_cornell_render_lattice_budget(&cam, fb, next_y, step, budget.value);
            } else {
                next_y = rt_refine_lattice_range_end(GFX_WIDTH, GFX_HEIGHT, next_y, step, budget.value);
                rt_cornell_render_rows(&cam, fb, first_y, next_y, step);
            }
            const int64_t trace_us = esp_timer_get_time() - trace_start;

            const int clamped_end = next_y < GFX_HEIGHT ? next_y : GFX_HEIGHT;
            gfx_mark_dirty(0, first_y, GFX_WIDTH, clamped_end - first_y);

            if (hud_on) {
                draw_fps(&null_input, false);
            }

            gfx_reset_strip_send_counts();
            const int64_t present_start = esp_timer_get_time();
            gfx_present();
            const int64_t present_us = esp_timer_get_time() - present_start;

            out[pass].trace_us += trace_us;
            out[pass].present_us += present_us;
            out[pass].transfers += gfx_get_transfer_count();
            out[pass].bytes += gfx_get_bytes_sent();
            out[pass].frames++;

            dt_ms = (uint32_t)((trace_us + present_us) / 1000);
        }
        next_y = 0;
        step = rt_refine_next_step(step);
    }
}

static void
assert_pass_did_real_work(int quarter, bool hud_on, bool dual_core, int pass, const pass_totals_t* t) {
    char why[96];
    snprintf(why, sizeof why, "quarter=%d hud=%d cores=%s pass=%d", quarter, hud_on, dual_core ? "dual" : "single",
             pass + 1);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, t->frames, why);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, t->transfers, why);
    TEST_ASSERT_TRUE_MESSAGE(t->trace_us > 0, why);
    TEST_ASSERT_TRUE_MESSAGE(t->bytes > 0, why);
}

void
test_cornell_present_cost_by_quarter_hud_and_cores(void) {
    ui_init(); /* draw_fps() needs ctx->text_width/text_height - see suite_cube_perf.c's fixture */

    ESP_LOGI(TAG,
             "=== CORNELL PRESENT PERF (%dx%d) - trace/present are clocks, frames/transfers/bytes are counters ===",
             GFX_WIDTH, GFX_HEIGHT);

    static const int quarters[] = {DISPLAY_PORTRAIT, DISPLAY_LANDSCAPE};
    static const bool hud_states[] = {true, false};
    static const bool core_states[] = {false, true}; /* single, then dual */

    pass_totals_t totals[RT_REFINE_PASSES];
    for (size_t q = 0; q < sizeof(quarters) / sizeof(quarters[0]); q++) {
        for (size_t h = 0; h < sizeof(hud_states) / sizeof(hud_states[0]); h++) {
            for (size_t c = 0; c < sizeof(core_states) / sizeof(core_states[0]); c++) {
                run_arm(quarters[q], hud_states[h], core_states[c], totals);
                for (int pass = 0; pass < RT_REFINE_PASSES; pass++) {
                    log_pass(quarters[q], hud_states[h], core_states[c], pass, &totals[pass]);
                    assert_pass_did_real_work(quarters[q], hud_states[h], core_states[c], pass, &totals[pass]);
                }
            }
        }
    }

    ui_set_transform(ui_transform_identity());
    TEST_PASS();
}

void
run_rt_present_perf_suite(void) {
    RUN_TEST(test_cornell_present_cost_by_quarter_hud_and_cores);
}

#else /* !DEVICE_BUILD */

void
run_rt_present_perf_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_rt_present_perf_suite);

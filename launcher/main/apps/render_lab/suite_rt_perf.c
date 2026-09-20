/*
 * Device-only suite: the Cornell tracer's own frame budget - each
 * refinement pass (rt_refine.h), the whole picture, and a plain row-by-row
 * trace through rt_cornell_render_row() as a reference with no block fills
 * and no present, isolating the tracer's per-pixel cost alone. Traces into
 * a private heap buffer, never the framebuffer.
 *
 * This is the first single-precision float measurement taken on this chip
 * - every microsecond below is float work, not this project's usual fixed
 * point, and is read as such.
 *
 * Runs under DEVICE_BUILD only - needs real internal RAM, PSRAM and a clock.
 */
#include "suites.h" /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdint.h>

#include "unity.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "gfx/gfx.h"
#include "rt_cornell.h"
#include "rt_refine.h"

static const char* TAG = "rt_perf";

/* Two orientations, not four: quarter only rolls the physical-to-upright
 * mapping (r3d_ray.h), never the ray count or the tracer's own cost, so 0
 * and 1 already cover both the swapped and unswapped axis case. */
#define QUARTER_COUNT 2

static void
fill_block(gfx_color_t* buf, int x, int y, int step, gfx_color_t color) {
    const int w = x + step > GFX_WIDTH ? GFX_WIDTH - x : step;
    const int h = y + step > GFX_HEIGHT ? GFX_HEIGHT - y : step;

    for (int row = 0; row < h; row++) {
        gfx_color_t* dst = buf + (size_t)(y + row) * GFX_WIDTH + x;
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

typedef struct {
    int64_t pass_us[RT_REFINE_PASSES];
    int64_t pass_pixels[RT_REFINE_PASSES];
    int64_t total_us;
    int64_t total_pixels;
} refine_result_t;

/* One trace of the whole picture, refinement pass by pass - scene_raytrace.c's
 * own shape (trace_lattice_row(), fill_block()), rebuilt over a private
 * buffer instead of the shell's framebuffer and timed pass by pass. */
static refine_result_t
run_refine_capture(const rt_cornell_camera_t* cam, gfx_color_t* buf) {
    refine_result_t r = {0};
    int step = RT_REFINE_FIRST_STEP;

    for (int pass = 0; pass < RT_REFINE_PASSES; pass++) {
        const int64_t pass_start = esp_timer_get_time();
        int64_t traced = 0;

        for (int y = 0; y < GFX_HEIGHT; y += step) {
            for (int x = 0; x < GFX_WIDTH; x += step) {
                if (!rt_refine_is_new(x, y, step)) {
                    continue;
                }
                fill_block(buf, x, y, step, rt_cornell_render_pixel(cam, x, y));
                traced++;
            }
        }

        r.pass_us[pass] = esp_timer_get_time() - pass_start;
        r.pass_pixels[pass] = traced;
        r.total_us += r.pass_us[pass];
        r.total_pixels += traced;
        step = rt_refine_next_step(step);
    }
    return r;
}

/* Every physical pixel, one row at a time, through the same entry point
 * scene_raytrace.c's non-progressive callers would use - no lattice, no
 * fill_block(), so this is the tracer's cost with nothing else added. */
static int64_t
run_row_reference(const rt_cornell_camera_t* cam, gfx_color_t* buf) {
    const int64_t start = esp_timer_get_time();

    for (int y = 0; y < GFX_HEIGHT; y++) {
        rt_cornell_render_row(cam, y, buf + (size_t)y * GFX_WIDTH);
    }
    return esp_timer_get_time() - start;
}

static void
log_refine_result(int quarter, const refine_result_t* r) {
    int step = RT_REFINE_FIRST_STEP;

    for (int pass = 0; pass < RT_REFINE_PASSES; pass++) {
        ESP_LOGI(TAG, "quarter=%d pass %d/%d step=%-2d %8lldus %6lldpx %6.3fus/px(float)", quarter, pass + 1,
                 RT_REFINE_PASSES, step, (long long)r->pass_us[pass], (long long)r->pass_pixels[pass],
                 (double)r->pass_us[pass] / (double)r->pass_pixels[pass]);
        step = rt_refine_next_step(step);
    }
    ESP_LOGI(TAG, "quarter=%d whole picture   %8lldus %6lldpx %6.3fus/px(float)", quarter, (long long)r->total_us,
             (long long)r->total_pixels, (double)r->total_us / (double)r->total_pixels);
}

static void
log_row_reference(int quarter, int64_t row_us) {
    const int64_t pixels = (int64_t)GFX_WIDTH * GFX_HEIGHT;
    ESP_LOGI(TAG, "quarter=%d row reference   %8lldus %6lldpx %6.3fus/px(float, no block fills, no present)", quarter,
             (long long)row_us, (long long)pixels, (double)row_us / (double)pixels);
}

/* rt_refine.h's own guarantee, exercised at the real panel size rather than
 * a suite-picked one: every pixel is traced by exactly one pass. */
static void
assert_every_pixel_traced_once(const refine_result_t* r) {
    for (int pass = 0; pass < RT_REFINE_PASSES; pass++) {
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)r->pass_pixels[pass], "a refinement pass traced nothing");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(GFX_WIDTH * GFX_HEIGHT, (int)r->total_pixels,
                                  "the four passes together must trace every pixel exactly once");
}

static void
run_quarter(int quarter) {
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    gfx_color_t* buf =
        heap_caps_malloc(sizeof(*buf) * (size_t)GFX_WIDTH * GFX_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    TEST_ASSERT_NOT_NULL_MESSAGE(buf, "need a private canvas the size of one frame");

    rt_cornell_camera_t cam;
    rt_cornell_camera_init(&cam, (r3d_viewport_t){GFX_WIDTH, GFX_HEIGHT, quarter});

    const refine_result_t refine = run_refine_capture(&cam, buf);
    log_refine_result(quarter, &refine);
    assert_every_pixel_traced_once(&refine);

    const int64_t row_us = run_row_reference(&cam, buf);
    log_row_reference(quarter, row_us);

    heap_caps_free(buf);
    const size_t free_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)free_before, (int)free_after, "the private canvas was not fully freed");
}

void
test_cornell_tracer_frame_budget_by_quarter(void) {
    ESP_LOGI(TAG, "=== CORNELL TRACER PERF (float, %dx%d) ===", GFX_WIDTH, GFX_HEIGHT);

    static const int quarters[QUARTER_COUNT] = {0, 1};
    for (int i = 0; i < QUARTER_COUNT; i++) {
        run_quarter(quarters[i]);
    }

    TEST_PASS();
}

void
run_rt_perf_suite(void) {
    RUN_TEST(test_cornell_tracer_frame_budget_by_quarter);
}

#else /* !DEVICE_BUILD */

void
run_rt_perf_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_rt_perf_suite);

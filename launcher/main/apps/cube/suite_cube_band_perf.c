/*
 * Device-only suite: the cube's band-mode path against its full-fb path, on
 * the same rotating scene - the A/B the band ring exists to answer (see
 * docs/Autana-Rendering-Roadmap.md section 3.3's frame-time argument).
 *
 * Both variants draw the same scene with no HUD and no partial-clear
 * bookkeeping, since band mode has neither: every band is a full redraw, so
 * the fairest full-fb comparison is also a full gfx_clear() and a full
 * gfx_present() every frame, not the app's own partial-update default.
 *
 * Runs under DEVICE_BUILD only - needs real internal RAM, PSRAM, DMA and the
 * panel.
 */
#include "suites.h" /* portable - needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "gfx/gfx.h"
#include "ui/ui.h"

/* app_cube.c's own toggle and lifecycle, exposed the same way
 * suite_cube_perf.c already relies on. */
extern bool cube_band_mode;
extern bool partial_updates;
extern void cube_enter(void);
extern void cube_exit(void);
extern void cube_update_rotation(uint32_t dt_ms);
extern void cube_clear_frame(void);
extern void cube_rasterize_frame(void);
extern void cube_transform_and_bin(void);
extern void cube_rasterize_band(gfx_color_t* buf, int row0, int row1);

static const char* TAG = "cube_band_perf";

#define SAMPLE_SECONDS 10
#define SAMPLE_MS      (SAMPLE_SECONDS * 1000)
#define MAX_SAMPLES    128

static int32_t* samples;
static int sample_count;

static int
cmp_i32(const void* a, const void* b) {
    const int32_t va = *(const int32_t*)a;
    const int32_t vb = *(const int32_t*)b;
    return (va > vb) - (va < vb);
}

typedef struct {
    int64_t min, max, avg, med;
} stats_t;

static stats_t
compute_stats(int n) {
    stats_t s = {.min = INT64_MAX, .max = 0, .avg = 0, .med = 0};
    int64_t sum = 0;

    for (int i = 0; i < n; i++) {
        if (samples[i] < s.min) {
            s.min = samples[i];
        }
        if (samples[i] > s.max) {
            s.max = samples[i];
        }
        sum += samples[i];
    }
    s.avg = sum / n;
    qsort(samples, n, sizeof(int32_t), cmp_i32);
    s.med = samples[n / 2];
    return s;
}

static void
log_stats(const char* label, stats_t s) {
    ESP_LOGI(TAG, "%-8s min=%lldus max=%lldus avg=%lldus med=%lldus (%.1f/%.1f fps avg/med)", label, (long long)s.min,
             (long long)s.max, (long long)s.avg, (long long)s.med, 1000000.0 / (double)s.avg,
             1000000.0 / (double)s.med);
}

/* Runs one variant for SAMPLE_SECONDS, timing whichever frame path
 * `run_frame` performs - either the band loop or the full-fb path, both
 * called with dt_ms clamped the same way main.c's own loop clamps it. */
static void
capture(void (*run_frame)(uint32_t dt_ms)) {
    sample_count = 0;
    int64_t start = esp_timer_get_time();
    int64_t next_due = start;

    while (esp_timer_get_time() - start < SAMPLE_MS * 1000) {
        int64_t frame_start = esp_timer_get_time();
        int64_t dt_ms = (frame_start - next_due) / 1000;
        if (dt_ms < 0) {
            dt_ms = 1;
        }
        if (dt_ms > 250) {
            dt_ms = 250;
        }
        next_due += dt_ms * 1000;

        run_frame((uint32_t)dt_ms);

        const int idx = sample_count % MAX_SAMPLES;
        samples[idx] = (int32_t)(esp_timer_get_time() - frame_start);
        sample_count++;
    }
}

static void
full_fb_frame(uint32_t dt_ms) {
    cube_update_rotation(dt_ms);
    cube_clear_frame();
    cube_rasterize_frame();
    gfx_present();
}

/* cube_frame_band()'s own shape, rebuilt from the pieces app_cube.c exposes
 * (its own clear_band() is file-static). cube_transform_and_bin() must run
 * once per frame, before the band loop, or the bin holds the previous
 * frame's triangles. */
static void
band_frame(uint32_t dt_ms) {
    const gfx_color_t bg = gfx_rgb(0x0A0C14);

    cube_update_rotation(dt_ms);
    cube_transform_and_bin();
    gfx_band_frame_begin();
    while (gfx_band_next()) {
        gfx_color_t* buf = gfx_band_buffer();
        const int row0 = gfx_band_row0();
        const int height = gfx_band_height();

        for (int i = 0; i < GFX_WIDTH * height; i++) {
            buf[i] = bg;
        }
        cube_rasterize_band(buf, row0, row0 + height);
        gfx_band_submit();
    }
}

void
test_cube_band_mode_against_full_fb_on_the_same_scene(void) {
    ui_init(); /* cube_enter() reads ui_layout_generation(); see suite_cube_perf.c's fixture */

    samples = malloc(sizeof(int32_t) * MAX_SAMPLES);
    TEST_ASSERT_NOT_NULL_MESSAGE(samples, "need a sample buffer for the band-vs-full-fb capture");

    /* Full redraw every frame, matching band mode's own shape - not the
     * app's real default, which would let partial updates skip most of the
     * clear and present. */
    partial_updates = false;
    cube_band_mode = false;
    cube_enter();
    capture(full_fb_frame);
    cube_exit();
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, sample_count, "no full-fb frames captured");
    const int full_fb_n = (sample_count < MAX_SAMPLES) ? sample_count : MAX_SAMPLES;
    const stats_t full_fb_stats = compute_stats(full_fb_n);

    cube_band_mode = true;
    cube_enter();
    capture(band_frame);
    cube_exit();
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, sample_count, "no band-mode frames captured");
    const int band_n = (sample_count < MAX_SAMPLES) ? sample_count : MAX_SAMPLES;
    const stats_t band_stats = compute_stats(band_n);

    cube_band_mode = false;

    ESP_LOGI(TAG, "=== CUBE BAND VS FULL-FB (%ds each, band height %d) ===", SAMPLE_SECONDS, GFX_BAND_HEIGHT);
    log_stats("full_fb", full_fb_stats);
    log_stats("band", band_stats);

    free(samples);
    samples = NULL;

    TEST_PASS();
}

void
run_cube_band_perf_suite(void) {
    RUN_TEST(test_cube_band_mode_against_full_fb_on_the_same_scene);
}

#else /* !DEVICE_BUILD */

void
run_cube_band_perf_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_cube_band_perf_suite);

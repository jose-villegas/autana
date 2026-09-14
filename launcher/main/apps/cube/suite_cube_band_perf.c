/*
 * Device-only suite: the cube's band-mode path against its full-fb path, on
 * the same rotating scene with the fps counter showing in both - the A/B
 * the band ring exists to answer (see docs/Autana-Rendering-Roadmap.md
 * section 3.3's frame-time argument), with real UI cost included rather
 * than measured separately.
 *
 * Neither variant uses the app's own partial-update default: band mode
 * has no partial-clear path at all (every band is a full redraw), so the
 * fairest full-fb comparison is also a full gfx_clear() and a full
 * gfx_present() every frame.
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

#include "display/display.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

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
extern void draw_fps(const input_t* input, bool for_bands);

static const char* TAG = "cube_band_perf";

#define SAMPLE_SECONDS        10
#define SAMPLE_MS             (SAMPLE_SECONDS * 1000)
#define MAX_SAMPLES           128

/* Shorter than SAMPLE_MS: the orientation/fps sweep below is 8 arms (2
 * render modes x 2 orientations x 2 fps states), and at 10 s apiece that
 * is 80 s just for this one test. 3 s still gives ~100 samples at the
 * frame rates this app runs. */
#define ORIENTATION_SAMPLE_MS (3 * 1000)

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

/* A band arm that skips every band looks identical to a healthy one by
 * sample_count alone - exactly how the all-skip regression this guards
 * against once passed silently (both tests only logged). The frame-time
 * floor catches "rendered impossibly fast to be real" the way a bare
 * touched>0 check would still miss a bug that only fires most frames. */
static void
assert_band_frame_did_real_work(stats_t frame, int touched, int64_t raster_us) {
    /* Total bytes across the whole capture, not a per-frame average: a
     * per-frame average this small can truncate to 0 as an integer even
     * when touched is genuinely nonzero, which would fail this for the
     * wrong reason. touched > 0 already implies this is positive - kept
     * as its own check because it is the sanity property item 3 named. */
    const int64_t bytes_sent = (int64_t)touched * GFX_WIDTH * GFX_BAND_HEIGHT * sizeof(gfx_color_t);

    /* Plain boolean asserts, not the *_INT64 comparison macros: this
     * device's Unity build has 64-bit support disabled, and those abort
     * with "Unity 64-bit Support Disabled" before printing anything -
     * see suite_fixed.c's own comment on the same constraint. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, touched, "band mode touched no bands - the cube never redraws");
    TEST_ASSERT_TRUE_MESSAGE(raster_us > 0, "band mode never rasterized a touched band");
    TEST_ASSERT_TRUE_MESSAGE(bytes_sent > 0, "band mode sent nothing to the panel");
    TEST_ASSERT_TRUE_MESSAGE(frame.avg > 2000, "band frame time is impossibly fast - bands are likely being skipped");
}

/* Runs one variant for `duration_ms`, timing whichever frame path
 * `run_frame` performs - either the band loop or the full-fb path, both
 * called with dt_ms clamped the same way main.c's own loop clamps it. */
static void
capture(void (*run_frame)(uint32_t dt_ms), int64_t duration_ms) {
    sample_count = 0;
    int64_t start = esp_timer_get_time();
    int64_t next_due = start;

    while (esp_timer_get_time() - start < duration_ms * 1000) {
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

/* Nothing pressed, no touch - draw_fps() feeds this straight into
 * ui_begin()/feed_input(), which dereference it unconditionally. */
static const input_t null_input = {0};

/* Time spent inside ui_replay_band() and how many bands that covers -
 * band mode's own per-band cost. touched_band_count/skipped_band_count
 * are gfx_band_dirty()'s decision (gfx.c). ui_build_us_accum times
 * draw_fps() itself, the once-per-frame cost both arms pay. */
static int64_t replay_us_accum;
static int replay_band_count;
static int touched_band_count;
static int skipped_band_count;
static int64_t ui_build_us_accum;

/* Total time inside cube_rasterize_band() across a capture - proof a
 * touched band actually rasterized something, not just bookkeeping. */
static int64_t raster_us_accum;

/* Whether either arm draws the fps counter at all this capture - off
 * isolates the cube's own cost from the UI's, on (the default) is what
 * every capture before the orientation sweep always measured. */
static bool run_fps_on = true;

static void
full_fb_frame(uint32_t dt_ms) {
    cube_update_rotation(dt_ms);
    cube_clear_frame();
    cube_rasterize_frame();
    if (run_fps_on) {
        const int64_t build_start = esp_timer_get_time();
        draw_fps(&null_input, false);
        ui_build_us_accum += esp_timer_get_time() - build_start;
    }
    gfx_present();
}

/* cube_frame_band()'s own shape, rebuilt from the pieces app_cube.c exposes
 * (its own clear_band() is file-static). cube_transform_and_bin() must run
 * once per frame, before the band loop - it also marks the cube's own
 * coverage dirty (its own comment), the only reason gfx_band_dirty() below
 * ever returns true for a rotating cube. draw_fps(for_bands=true) builds
 * the HUD's commands once, for ui_replay_band() to bin per band below. */
static void
band_frame(uint32_t dt_ms) {
    const gfx_color_t bg = gfx_rgb(0x0A0C14);

    cube_update_rotation(dt_ms);
    cube_transform_and_bin();
    if (run_fps_on) {
        const int64_t build_start = esp_timer_get_time();
        draw_fps(&null_input, true);
        ui_build_us_accum += esp_timer_get_time() - build_start;
    }

    gfx_band_frame_begin();
    while (gfx_band_next()) {
        const int row0 = gfx_band_row0();
        const int height = gfx_band_height();

        int x0, x1;
        if (!gfx_band_dirty(row0, row0 + height, &x0, &x1)) {
            gfx_band_skip();
            skipped_band_count++;
            continue;
        }
        (void)x0;
        (void)x1;
        touched_band_count++;

        gfx_color_t* buf = gfx_band_buffer();
        for (int i = 0; i < GFX_WIDTH * height; i++) {
            buf[i] = bg;
        }
        const int64_t raster_start = esp_timer_get_time();
        cube_rasterize_band(buf, row0, row0 + height);
        raster_us_accum += esp_timer_get_time() - raster_start;

        if (run_fps_on) {
            const int64_t replay_start = esp_timer_get_time();
            ui_replay_band(row0, row0 + height);
            replay_us_accum += esp_timer_get_time() - replay_start;
            replay_band_count++;
        }

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
    run_fps_on = true;
    cube_band_mode = false;
    cube_enter();
    capture(full_fb_frame, SAMPLE_MS);
    cube_exit();
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, sample_count, "no full-fb frames captured");
    const int full_fb_n = (sample_count < MAX_SAMPLES) ? sample_count : MAX_SAMPLES;
    const stats_t full_fb_stats = compute_stats(full_fb_n);

    cube_band_mode = true;
    replay_us_accum = 0;
    replay_band_count = 0;
    touched_band_count = 0;
    skipped_band_count = 0;
    raster_us_accum = 0;
    cube_enter();
    capture(band_frame, SAMPLE_MS);
    cube_exit();
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, sample_count, "no band-mode frames captured");
    const int band_n = (sample_count < MAX_SAMPLES) ? sample_count : MAX_SAMPLES;
    const stats_t band_stats = compute_stats(band_n);
    const double band_bytes_per_frame =
        (double)touched_band_count * GFX_WIDTH * GFX_BAND_HEIGHT * sizeof(gfx_color_t) / sample_count;

    cube_band_mode = false;

    ESP_LOGI(TAG, "=== CUBE BAND VS FULL-FB (%ds each, band height %d, fps counter on in both) ===", SAMPLE_SECONDS,
             GFX_BAND_HEIGHT);
    log_stats("full_fb", full_fb_stats);
    log_stats("band", band_stats);
    if (replay_band_count > 0) {
        ESP_LOGI(TAG, "ui_replay_band: %lld us total over %d bands, %.1f us/band, %.1f us/frame",
                 (long long)replay_us_accum, replay_band_count, (double)replay_us_accum / replay_band_count,
                 (double)replay_us_accum / sample_count);
    }
    {
        const int total_bands = touched_band_count + skipped_band_count;
        const double touched_pct = total_bands > 0 ? 100.0 * touched_band_count / total_bands : 0.0;
        ESP_LOGI(TAG, "bands: %d touched, %d skipped (%.1f%% touched), %.0f bytes/frame sent", touched_band_count,
                 skipped_band_count, touched_pct, band_bytes_per_frame);
    }

    /* After the log lines, not before: a failing arm must still print its
     * numbers - the all-skip regression this guards against would
     * otherwise abort silently, the same blind spot logging-only once had. */
    assert_band_frame_did_real_work(band_stats, touched_band_count, raster_us_accum);

    free(samples);
    samples = NULL;

    TEST_PASS();
}

/*
 * Orientation x fps sweep - item 3's own ask: the cube ran faster in
 * portrait than landscape on the board, and the suspect was the fps
 * counter's rotated text and the extra bands its box spans once rotated
 * (see display.h for the quarter numbering forced here). Each arm below
 * isolates one axis of that: render mode (full_fb/band), orientation,
 * and whether the counter is drawn at all.
 */

typedef struct {
    const char* label;
    stats_t frame;
    int frame_count; /* the real, uncapped sample_count - compute_stats()'s own
                       * n is capped to MAX_SAMPLES for the ring buffer, which
                       * would badly inflate a per-frame total divided by it at
                       * the frame rates an all-skip band bug runs at */
    int64_t ui_build_us;
    int64_t replay_us;
    int replay_band_count;
    int64_t raster_us;
    int touched_bands;
    int skipped_bands;
} arm_result_t;

static arm_result_t
run_arm(const char* label, bool band_mode, int quarter, bool fps_on) {
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));

    run_fps_on = fps_on;
    ui_build_us_accum = 0;
    replay_us_accum = 0;
    replay_band_count = 0;
    touched_band_count = 0;
    skipped_band_count = 0;
    raster_us_accum = 0;

    cube_band_mode = band_mode;
    cube_enter();
    capture(band_mode ? band_frame : full_fb_frame, ORIENTATION_SAMPLE_MS);
    cube_exit();
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, sample_count, "no frames captured for this arm");

    arm_result_t r = {
        .label = label,
        .frame_count = sample_count,
        .ui_build_us = ui_build_us_accum,
        .replay_us = replay_us_accum,
        .replay_band_count = replay_band_count,
        .raster_us = raster_us_accum,
        .touched_bands = touched_band_count,
        .skipped_bands = skipped_band_count,
    };
    const int n = (sample_count < MAX_SAMPLES) ? sample_count : MAX_SAMPLES;
    r.frame = compute_stats(n);
    return r;
}

static void
log_arm(const arm_result_t* r) {
    log_stats(r->label, r->frame);
    ESP_LOGI(TAG,
             "%-24s ui_build=%.1f us/frame, ui_replay=%.1f us/frame, bands touched=%d skipped=%d, "
             "%.0f bytes/frame sent",
             r->label, (double)r->ui_build_us / r->frame_count,
             r->replay_band_count > 0 ? (double)r->replay_us / r->frame_count : 0.0, r->touched_bands, r->skipped_bands,
             (double)r->touched_bands * GFX_WIDTH * GFX_BAND_HEIGHT * sizeof(gfx_color_t) / r->frame_count);
}

void
test_cube_orientation_and_fps_sweep(void) {
    ui_init();
    samples = malloc(sizeof(int32_t) * MAX_SAMPLES);
    TEST_ASSERT_NOT_NULL_MESSAGE(samples, "need a sample buffer for the orientation/fps sweep");
    partial_updates = false;

    static const struct {
        const char* name;
        int quarter;
    } orientations[] = {
        {"portrait", DISPLAY_PORTRAIT},
        {"landscape", DISPLAY_LANDSCAPE},
    };

    static const bool fps_states[] = {true, false};

    ESP_LOGI(TAG,
             "=== CUBE ORIENTATION x FPS SWEEP (%lds/arm, band height %d) ===", (long)(ORIENTATION_SAMPLE_MS / 1000),
             GFX_BAND_HEIGHT);
    for (size_t o = 0; o < sizeof(orientations) / sizeof(orientations[0]); o++) {
        for (size_t f = 0; f < sizeof(fps_states) / sizeof(fps_states[0]); f++) {
            char label[32];

            snprintf(label, sizeof label, "full_fb/%s/fps-%s", orientations[o].name, fps_states[f] ? "on" : "off");
            arm_result_t full = run_arm(label, false, orientations[o].quarter, fps_states[f]);
            log_arm(&full);

            snprintf(label, sizeof label, "band/%s/fps-%s", orientations[o].name, fps_states[f] ? "on" : "off");
            arm_result_t band = run_arm(label, true, orientations[o].quarter, fps_states[f]);
            log_arm(&band);
            /* After logging, not inside run_arm(): a failing arm must
             * still print its numbers first. */
            assert_band_frame_did_real_work(band.frame, band.touched_bands, band.raster_us);
        }
    }

    cube_band_mode = false;
    ui_set_transform(ui_transform_identity());

    free(samples);
    samples = NULL;
    TEST_PASS();
}

void
run_cube_band_perf_suite(void) {
    RUN_TEST(test_cube_band_mode_against_full_fb_on_the_same_scene);
    RUN_TEST(test_cube_orientation_and_fps_sweep);
}

#else /* !DEVICE_BUILD */

void
run_cube_band_perf_suite(void) {}

#endif /* DEVICE_BUILD */

SUITE_REGISTER(run_cube_band_perf_suite);

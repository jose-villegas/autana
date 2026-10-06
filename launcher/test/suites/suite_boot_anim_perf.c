/*
 * Device-only suite: boot animation performance profiling.
 *
 * Unlike a steady-state render loop, boot_anim_draw_frame() is a pure
 * function of now_ms, and the whole point of this suite
 * is that its cost is NOT steady across the animation's own timeline: grid
 * rings arrive progressively, the space transform's scale grows across
 * keyframes, and the photograph's crossfade adds a full-framebuffer blend
 * on top of whatever else is drawing. A single flat-out capture, the way a
 * steady-state suite takes one, would average all of that away.
 *
 * So this FREEZES time instead of letting it run: boot_anim_draw_frame()
 * being a pure function of now_ms means calling it repeatedly at one fixed
 * timestamp is a legitimate, repeatable measurement of exactly what that
 * moment in the animation costs, not a hand-picked scene standing in for
 * it. A handful of checkpoints (see build_checkpoints() below), each timed
 * per-phase (clear/floor/axes/curve/zeros/image/title/present), with the
 * same min/max/avg/median/p95 report every performance suite here reports.
 *
 * Runs under DEVICE_BUILD only: needs real panel, DMA, and framebuffer.
 */
#include "suites.h" /* portable: needed by SUITE_REGISTER() even on host */

#ifdef DEVICE_BUILD

#include "perf_stats.h"

#include <stdint.h>
#include <stdlib.h>

#include "unity.h"

#include "esp_log.h"

#include "boot/boot_anim.h"
#include "boot/boot_anim_timeline.h"
#include "gfx/gfx.h"
#include "util/runtime/timing.h"

/* boot_anim.c's own draw_* functions, exposed specifically for this suite;
 * see boot_anim.c's own comment above draw_floor() for why they are
 * non-static rather than declared in boot_anim.h. Calling these directly,
 * not a hand-copy of boot_anim_draw_frame()'s own sequencing, means this
 * suite exercises the exact code a real frame runs. */
extern void boot_anim_clear_frame(void);
extern void draw_floor(uint32_t now_ms, uint8_t ink, const boot_anim_view_t* view);
extern void draw_axes(uint32_t now_ms, uint8_t ink, const boot_anim_view_t* view);
extern int32_t draw_curve(uint32_t now_ms, uint8_t ink, const boot_anim_view_t* view);
extern void draw_zeros(int32_t pen_t_q8, uint8_t ink, const boot_anim_view_t* view);
extern void draw_image(uint8_t ink, uint8_t reveal);
extern void draw_title(uint32_t now_ms, uint8_t ink);

static const char* TAG = "boot_anim_perf";

/* Each checkpoint freezes now_ms and repeats the frame this many times. The
 * workload is deterministic per checkpoint, so a fixed count carries as much
 * data as a time-bounded ring would, and seven checkpoints still finish in
 * well under a minute. */
#define SAMPLES_PER_CHECKPOINT 60

typedef struct {
    int32_t frame_total_us;
    int32_t clear_us;
    int32_t floor_us;
    int32_t axes_us;
    int32_t curve_us;
    int32_t zeros_us;
    int32_t image_us;
    int32_t title_us;
    int32_t present_us;
} frame_sample_t;

/* Heap, freed after each checkpoint's report: a selftest run walks every
 * suite in one boot, so a static array would be .bss paid for the whole run,
 * not just while this suite executes. */
static frame_sample_t* samples = NULL;
static int32_t* stat_scratch = NULL;

typedef enum {
    FIELD_TOTAL,
    FIELD_CLEAR,
    FIELD_FLOOR,
    FIELD_AXES,
    FIELD_CURVE,
    FIELD_ZEROS,
    FIELD_IMAGE,
    FIELD_TITLE,
    FIELD_PRESENT,
} sample_field_t;

static int32_t
field_of(const frame_sample_t* s, sample_field_t field) {
    switch (field) {
        case FIELD_TOTAL: return s->frame_total_us;
        case FIELD_CLEAR: return s->clear_us;
        case FIELD_FLOOR: return s->floor_us;
        case FIELD_AXES: return s->axes_us;
        case FIELD_CURVE: return s->curve_us;
        case FIELD_ZEROS: return s->zeros_us;
        case FIELD_IMAGE: return s->image_us;
        case FIELD_TITLE: return s->title_us;
        case FIELD_PRESENT: return s->present_us;
    }
    return 0;
}

static int32_t*
sample_values(sample_field_t field, int n) {
    for (int i = 0; i < n; i++) {
        stat_scratch[i] = field_of(&samples[i], field);
    }
    return stat_scratch;
}

typedef struct {
    const char* label;
    uint32_t now_ms;
} checkpoint_t;

static uint32_t
clamp_below(uint32_t ms, uint32_t exclusive_max) {
    if (exclusive_max == 0) {
        return 0;
    }
    return ms < exclusive_max ? ms : exclusive_max - 1;
}

/* Every checkpoint is derived from the generated timeline constants, never a
 * raw ms literal: the timeline is actively tuned, and a literal would stop
 * meaning what its label says at the next retune.
 *
 * "crossfade_late" is the worst case: a nearly fully grown curve still
 * overlapping the per-pixel dithered image loop, which runs only while
 * reveal is strictly between 0 and 255. It sits at 90% through the window,
 * not at the edge, so tween rounding cannot drop the frame into the cheap
 * memcpy branch. */
static void
build_checkpoints(checkpoint_t out[7]) {
    const uint32_t grid_settled =
        BOOT_ANIM_GRID_START_MS + (uint32_t)BOOT_ANIM_GRID_RINGS * BOOT_ANIM_GRID_RING_MS + BOOT_ANIM_GRID_FADE_MS;
    const uint32_t image_start = (uint32_t)BOOT_ANIM_IMAGE_START_MS;
    const uint32_t image_end = image_start + (uint32_t)BOOT_ANIM_IMAGE_FADE_MS;

    out[0] = (checkpoint_t){"curve_climbing_early",
                            clamp_below(BOOT_ANIM_PEN_START_MS + BOOT_ANIM_PEN_MS / 2, BOOT_ANIM_MS)};
    out[1] = (checkpoint_t){"title_flying_in", clamp_below(BOOT_ANIM_TITLE_START_MS + 200, BOOT_ANIM_MS)};
    out[2] = (checkpoint_t){"grid_settled_pre_image",
                            clamp_below(grid_settled, image_start < BOOT_ANIM_MS ? image_start : BOOT_ANIM_MS)};
    out[3] = (checkpoint_t){"crossfade_start", clamp_below(image_start + 50, BOOT_ANIM_MS)};
    out[4] = (checkpoint_t){"crossfade_mid", clamp_below(image_start + BOOT_ANIM_IMAGE_FADE_MS / 2, BOOT_ANIM_MS)};
    out[5] = (checkpoint_t){"crossfade_late", clamp_below(image_start + (BOOT_ANIM_IMAGE_FADE_MS * 9) / 10,
                                                          image_end < BOOT_ANIM_MS ? image_end : BOOT_ANIM_MS)};
    out[6] = (checkpoint_t){"near_end", clamp_below(BOOT_ANIM_MS > 100 ? BOOT_ANIM_MS - 100 : 0, BOOT_ANIM_MS)};
}

/* What one frozen moment draws with. Sampled, drawn and reported in three
 * separate frames: suites run on the main task, and the timeline sample and
 * the nine stats would otherwise stay on its stack under every draw call. */
typedef struct {
    uint32_t now_ms;
    uint8_t ink;
    uint8_t reveal;
    bool draw_scene;
    bool draw_title;
    boot_anim_view_t view;
} checkpoint_frame_t;

static __attribute__((noinline)) void
sample_checkpoint(const boot_anim_motion_t* motion, uint32_t now_ms, checkpoint_frame_t* f) {
    f->now_ms = now_ms;
    f->ink = boot_anim_ink(now_ms);
    f->reveal = boot_anim_image_reveal(now_ms);
    f->draw_scene = boot_anim_scene_reach(now_ms) > 0;
    f->draw_title = now_ms >= BOOT_ANIM_TITLE_START_MS;
    f->view = boot_anim_view(motion, GFX_WIDTH, GFX_HEIGHT, now_ms);
}

static __attribute__((noinline)) void
time_frames(const checkpoint_frame_t* f) {
    for (int i = 0; i < SAMPLES_PER_CHECKPOINT; i++) {
        int64_t t0, t1;
        frame_sample_t* s = &samples[i];
        const int64_t frame_start = timing_now_us();

        t0 = timing_now_us();
        boot_anim_clear_frame();
        t1 = timing_now_us();
        s->clear_us = (int32_t)(t1 - t0);

        if (f->draw_scene) {
            t0 = timing_now_us();
            draw_floor(f->now_ms, f->ink, &f->view);
            t1 = timing_now_us();
            s->floor_us = (int32_t)(t1 - t0);

            t0 = timing_now_us();
            draw_axes(f->now_ms, f->ink, &f->view);
            t1 = timing_now_us();
            s->axes_us = (int32_t)(t1 - t0);

            t0 = timing_now_us();
            const int32_t reached = draw_curve(f->now_ms, f->ink, &f->view);
            t1 = timing_now_us();
            s->curve_us = (int32_t)(t1 - t0);

            t0 = timing_now_us();
            draw_zeros(reached, f->ink, &f->view);
            t1 = timing_now_us();
            s->zeros_us = (int32_t)(t1 - t0);
        } else {
            s->floor_us = 0;
            s->axes_us = 0;
            s->curve_us = 0;
            s->zeros_us = 0;
        }

        t0 = timing_now_us();
        draw_image(f->ink, f->reveal);
        t1 = timing_now_us();
        s->image_us = (int32_t)(t1 - t0);

        if (f->draw_title) {
            t0 = timing_now_us();
            draw_title(f->now_ms, f->ink);
            t1 = timing_now_us();
            s->title_us = (int32_t)(t1 - t0);
        } else {
            s->title_us = 0;
        }

        t0 = timing_now_us();
        gfx_present();
        t1 = timing_now_us();
        s->present_us = (int32_t)(t1 - t0);

        s->frame_total_us = (int32_t)(t1 - frame_start);
    }
}

typedef struct {
    const char* label; /* one width, so the columns line up */
    sample_field_t field;
    bool spread; /* min/max/med/p95 as well as the average */
} phase_row_t;

static const phase_row_t PHASE_ROWS[] = {
    {"Clear:  ", FIELD_CLEAR, false}, {"Floor:  ", FIELD_FLOOR, false},  {"Axes:   ", FIELD_AXES, false},
    {"Curve:  ", FIELD_CURVE, false}, {"Zeros:  ", FIELD_ZEROS, false},  {"Image:  ", FIELD_IMAGE, true},
    {"Title:  ", FIELD_TITLE, false}, {"Present:", FIELD_PRESENT, true},
};

/* One phase's stats live only while its own line prints. */
static __attribute__((noinline)) void
log_phase(const phase_row_t* row, int64_t total_avg) {
    const perf_stats_t s =
        perf_stats_compute(sample_values(row->field, SAMPLES_PER_CHECKPOINT), SAMPLES_PER_CHECKPOINT);
    const double share = (double)s.avg / total_avg * 100;
    if (row->spread) {
        ESP_LOGI(TAG, "%s min=%lldus max=%lldus avg=%lldus med=%lldus p95=%lldus (%.1f%%)", row->label,
                 (long long)s.min, (long long)s.max, (long long)s.avg, (long long)s.med, (long long)s.p95, share);
    } else {
        ESP_LOGI(TAG, "%s avg=%lldus (%.1f%%)", row->label, (long long)s.avg, share);
    }
}

static __attribute__((noinline)) void
report_checkpoint(const checkpoint_t* cp) {
    const perf_stats_t total =
        perf_stats_compute(sample_values(FIELD_TOTAL, SAMPLES_PER_CHECKPOINT), SAMPLES_PER_CHECKPOINT);

    ESP_LOGI(TAG, "=== BOOT_ANIM PERF %s (now_ms=%u, %d samples) ===", cp->label, (unsigned)cp->now_ms,
             SAMPLES_PER_CHECKPOINT);
    ESP_LOGI(TAG, "Total:   min=%lldus max=%lldus avg=%lldus med=%lldus p95=%lldus (%.1f/%.1f/%.1f fps)",
             (long long)total.min, (long long)total.max, (long long)total.avg, (long long)total.med,
             (long long)total.p95, 1000000.0 / total.avg, 1000000.0 / total.med, 1000000.0 / total.p95);
    for (size_t i = 0; i < sizeof PHASE_ROWS / sizeof PHASE_ROWS[0]; i++) {
        log_phase(&PHASE_ROWS[i], total.avg);
    }
}

static void
run_checkpoint(const boot_anim_motion_t* motion, const checkpoint_t* cp) {
    samples = malloc(sizeof(frame_sample_t) * SAMPLES_PER_CHECKPOINT);
    stat_scratch = malloc(sizeof(int32_t) * SAMPLES_PER_CHECKPOINT);
    if (samples == NULL || stat_scratch == NULL) {
        free(samples);
        free(stat_scratch);
        samples = NULL;
        stat_scratch = NULL;
        TEST_FAIL_MESSAGE("need samples and stat_scratch buffers for the "
                          "boot_anim perf capture, and at least one of the "
                          "two failed to allocate");
    }

    checkpoint_frame_t frame;
    sample_checkpoint(motion, cp->now_ms, &frame);
    time_frames(&frame);
    report_checkpoint(cp);

    free(samples);
    free(stat_scratch);
    samples = NULL;
    stat_scratch = NULL;
}

void
test_boot_anim_performance_by_checkpoint(void) {
#if CONFIG_LAUNCHER_QEMU
    TEST_IGNORE_MESSAGE("performance requires the device clock and display");
#endif
    gfx_clear_clip();
    gfx_set_partial_clear(false);
    gfx_invalidate();

    checkpoint_t checkpoints[7];
    build_checkpoints(checkpoints);

    /* What boot draws through, loaded as boot loads it. */
    boot_anim_motion_t motion;
    boot_anim_motion_load(&motion);
    if (!motion.from_pack) {
        TEST_FAIL_MESSAGE("the boot clip did not load: this would time the rest pose");
    }
    for (int i = 0; i < 7; i++) {
        run_checkpoint(&motion, &checkpoints[i]);
    }
    boot_anim_motion_release(&motion);

    TEST_PASS();
}

void
run_boot_anim_perf_suite(void) {
    RUN_TEST(test_boot_anim_performance_by_checkpoint);
}

#else /* !DEVICE_BUILD */

void
run_boot_anim_perf_suite(void) {}

#endif

SUITE_REGISTER(run_boot_anim_perf_suite);

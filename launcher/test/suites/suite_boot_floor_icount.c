#include "build/build_variant.h"

#if defined(CONFIG_LAUNCHER_QEMU) || !defined(DEVICE_BUILD)
/* The host counts calls; QEMU measures totals and calibrates a line cost model. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
/* Counting has no timing claim; outlining bounds the host reference's stack. */
#if !defined(DEVICE_BUILD) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("no-inline")
#endif
#include "boot/boot_anim.h"
#if !defined(DEVICE_BUILD) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
#include "core/timing.h"
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "gfx/target/gfx_segment_clip.h"
#include "suites.h"
#include "testkit/bench.h"
#include "unity.h"

#define FLOOR_CROSSFADE_MS 3550
#define FLOOR_EDGE_PEAK_MS 3724
#define FLOOR_TIMING_REPS  64
#define FLOOR_LINE_REPS    32768
#define FLOOR_LINE_COLOR   RGB565BE(0xFFFFFF)
#define FLOOR_LINE_LENGTH  64
#define FLOOR_LINE_X       100
#define FLOOR_ROW_PREFIX   "boot_floor_"
#define CURVE_ROW_PREFIX   "boot_curve_"
#define FLOOR_TOTAL_ROW    "total"
#define FLOOR_REJECT_ROW   "ink0"
#define FLOOR_POINT_ROW    "line_point"
#define FLOOR_LONG_ROW     "line_long"
#define FLOOR_LINE_Y       100

extern void draw_floor(uint32_t now_ms, uint8_t ink, uint8_t reveal, const boot_anim_view_t* view);
extern int32_t draw_curve(uint32_t now_ms, uint8_t ink, const boot_anim_view_t* view);

#ifndef DEVICE_BUILD
#define FLOOR_COUNT_PREFIX "FLOOR_COUNTS"

#include "gfx/present/gfx_test.h"
#include "image/pixel/bayer.h"
#include "image/pixel/rgb565.h"
#include "testkit/cleanup.h"

static bool floor_counting;
static unsigned floor_edges;
static unsigned floor_pixels;

extern void __real_draw_line_ex(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags);

void
__wrap_draw_line_ex(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags) {
    __real_draw_line_ex(x0, y0, x1, y1, color, flags);
    if (!floor_counting) {
        return;
    }
    floor_edges++;
    const int start_x = x0;
    const int start_y = y0;
    const recti_t clip = GFX_SCREEN_RECT;
    if (gfx_clip_segment(&clip, &x0, &y0, &x1, &y1)) {
        const unsigned count = (unsigned)mathi_max(mathi_abs(x1 - x0), mathi_abs(y1 - y0)) + 1u;
        const unsigned on = (flags >> DRAW_LINE_DASH_ON_BIT) & DRAW_LINE_DASH_MASK;
        const unsigned off = (flags >> DRAW_LINE_DASH_OFF_BIT) & DRAW_LINE_DASH_MASK;
        const unsigned period = (flags & DRAW_LINE_DASHED) ? on + off : 0;
        const unsigned offset = (unsigned)mathi_max(mathi_abs(x0 - start_x), mathi_abs(y0 - start_y));
        for (unsigned sample = 0; sample < count; sample++) {
            const bool visible = !period || ((offset + sample) % period) < on;
            floor_pixels += visible && !(sample == 0 && (flags & DRAW_LINE_OPEN));
        }
    }
}

static void
fixture(void) {
    floor_edges = 0;
    floor_pixels = 0;
}

#ifdef BOOT_ANIM_MESH_CELLS
static bool
node_projects(uint32_t now_ms, int i, int j, int stride) {
    (void)now_ms;
    const int64_t step = BOOT_ANIM_MESH_STEP_Q12;
    const int64_t square = ((int64_t)i * i) + ((int64_t)j * j);
    const int64_t reach = ((int64_t)BOOT_ANIM_GRID_FADE * BOOT_ANIM_GRID_STEP_Q12) + (stride * step);
    if (square * step * step > reach * reach) {
        return false;
    }
    return true;
}

static __attribute__((noinline)) unsigned
projected_nodes(uint32_t now_ms, uint8_t reveal) {
    const int half = BOOT_ANIM_MESH_CELLS / 2;
    const int stride = bayer_level(reveal) >= BOOT_ANIM_DISSOLVE_COARSE_LEVEL ? 2 : 1;
    unsigned count = 0;
    for (int j = -half; j <= half; j += stride) {
        for (int i = -half; i <= half; i += stride) {
            count += node_projects(now_ms, i, j, stride);
        }
    }
    return count;
}
#else
static __attribute__((noinline)) unsigned
projected_nodes(uint32_t now_ms, uint8_t reveal) {
    (void)now_ms;
    (void)reveal;
    return 0;
}
#endif

static unsigned
count_frame(const boot_anim_motion_t* motion, uint32_t now_ms, bool report) {
    fixture();
    const boot_anim_view_t view = boot_anim_view(motion, GFX_WIDTH, GFX_HEIGHT, now_ms);
    const uint8_t reveal = boot_anim_photo_reveal(now_ms);
    if (reveal < 255) {
        draw_floor(now_ms, boot_anim_ink(now_ms), reveal, &view);
    }
    if (report) {
        printf(FLOOR_COUNT_PREFIX " ms=%" PRIu32 " nodes=%u edges=%u pixels=%u\n", now_ms,
               projected_nodes(now_ms, reveal), floor_edges, floor_pixels);
    }
    return floor_edges;
}

static void
test_floor_timeline_edge_counts(void) {
    fixture();
    TEST_ASSERT_TRUE(gfx_init());
    suite_set_test_cleanup(gfx_reset_for_test);
    boot_anim_motion_t motion;
    boot_anim_motion_load(&motion);
    TEST_ASSERT_TRUE(motion.from_pack);
    floor_counting = true;
    unsigned maximum = 0;
    uint32_t maximum_ms = 0;
    for (uint32_t now_ms = 0; now_ms < BOOT_ANIM_MS; now_ms++) {
        const unsigned edges = count_frame(&motion, now_ms, false);
        if (edges > maximum) {
            maximum = edges;
            maximum_ms = now_ms;
        }
    }
    printf("FLOOR_MAX ms=%" PRIu32 " edges=%u\n", maximum_ms, maximum);
    (void)count_frame(&motion, FLOOR_EDGE_PEAK_MS, true);
    (void)count_frame(&motion, FLOOR_CROSSFADE_MS, true);
    floor_counting = false;
    boot_anim_motion_release(&motion);
}
#endif

#ifdef DEVICE_BUILD

static void
report_total(const boot_anim_motion_t* motion, uint32_t now_ms, bool zero_ink) {
    const boot_anim_view_t view = boot_anim_view(motion, GFX_WIDTH, GFX_HEIGHT, now_ms);
    const uint8_t ink = zero_ink ? 0 : boot_anim_ink(now_ms);
    const uint8_t reveal = boot_anim_photo_reveal(now_ms);
    const int64_t start = timing_now_us();
    for (int rep = 0; rep < FLOOR_TIMING_REPS; rep++) {
        /* As after the frame's clear: lines mark nothing more. */
        gfx_mark_all_dirty();
        draw_floor(now_ms, ink, reveal, &view);
    }
    const int64_t elapsed = timing_now_us() - start;
    char name[64];
    (void)snprintf(name, sizeof name, FLOOR_ROW_PREFIX "%" PRIu32 "_%s", now_ms,
                   zero_ink ? FLOOR_REJECT_ROW : FLOOR_TOTAL_ROW);
    bench_icount_report(name, elapsed, FLOOR_TIMING_REPS, 1);
}

/* The curve over the same frames, after the frame's clear has marked
 * everything dirty, as boot draws it. */
static void
report_curve(const boot_anim_motion_t* motion, uint32_t now_ms) {
    const boot_anim_view_t view = boot_anim_view(motion, GFX_WIDTH, GFX_HEIGHT, now_ms);
    const uint8_t ink = boot_anim_ink(now_ms);
    const int64_t start = timing_now_us();
    for (int rep = 0; rep < FLOOR_TIMING_REPS; rep++) {
        gfx_mark_all_dirty();
        (void)draw_curve(now_ms, ink, &view);
    }
    const int64_t elapsed = timing_now_us() - start;
    char name[64];
    (void)snprintf(name, sizeof name, CURVE_ROW_PREFIX "%" PRIu32, now_ms);
    bench_icount_report(name, elapsed, FLOOR_TIMING_REPS, 1);
}

static void
report_line_cost(int length) {
    const int64_t start = timing_now_us();
    for (int rep = 0; rep < FLOOR_LINE_REPS; rep++) {
        gfx_mark_all_dirty();
        draw_line_ex(FLOOR_LINE_X, FLOOR_LINE_Y, FLOOR_LINE_X + length - 1, FLOOR_LINE_Y, FLOOR_LINE_COLOR, 0);
    }
    const int64_t elapsed = timing_now_us() - start;
    bench_icount_report(length == 1 ? FLOOR_ROW_PREFIX FLOOR_POINT_ROW : FLOOR_ROW_PREFIX FLOOR_LONG_ROW, elapsed,
                        FLOOR_LINE_REPS, 1);
}

static void
test_floor_instruction_rows(void) {
    boot_anim_motion_t motion;
    boot_anim_motion_load(&motion);
    TEST_ASSERT_TRUE(motion.from_pack);
    static const uint32_t frames_ms[] = {2464, FLOOR_CROSSFADE_MS, FLOOR_EDGE_PEAK_MS, 4050, 4372};
    for (unsigned frame = 0; frame < sizeof frames_ms / sizeof frames_ms[0]; frame++) {
        report_total(&motion, frames_ms[frame], false);
        report_total(&motion, frames_ms[frame], true);
        report_curve(&motion, frames_ms[frame]);
    }
    boot_anim_motion_release(&motion);
    report_line_cost(1);
    report_line_cost(FLOOR_LINE_LENGTH);
}
#endif

void
run_boot_floor_cost_suite(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_floor_timeline_edge_counts);
#else
    RUN_TEST(test_floor_instruction_rows);
#endif
}

SUITE_REGISTER_ON_REQUEST(run_boot_floor_cost_suite);
SUITE_READS(run_boot_floor_cost_suite, BOOT_CLIP);
#endif

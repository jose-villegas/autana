/*
 * Portable suite: the row-range split that puts scene_raytrace.c's lattice
 * pass and scene_pathtrace.c's seed and sweep passes on both cores
 * (rt_refine.h's range-end/split-point arithmetic, rt_cornell_render_rows(),
 * rt_path_seed_rows(), rt_path_sweep_rows()). THE DECISIVE CLAIM: a row's
 * colour depends only on (camera, x, y) or (camera, x, y, sample index),
 * never on which half of a split traced it - checked by rendering a range
 * whole and split at every row-aligned point, including both degenerate
 * splits, and comparing the framebuffers byte for byte. A core-1 job runs
 * inline on a host (util/job.h), so this exercises the same split the real
 * dispatch takes, not a stand-in for it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "apps/render_lab/rt_cornell.h"
#include "apps/render_lab/rt_path.h"
#include "apps/render_lab/rt_refine.h"

/* Partition arithmetic - pure, no tracing */

static void
test_split_mid_covers_every_row_exactly_once(void) {
    for (int step = 1; step <= 8; step *= 2) {
        for (int rows = 0; rows <= 20; rows++) {
            const int y0 = 3 * step; /* nonzero start, still on the lattice */
            const int y1 = y0 + rows * step;
            const int mid = rt_refine_split_mid(y0, y1, step);

            TEST_ASSERT_TRUE_MESSAGE(mid >= y0 && mid <= y1, "the split point left the range");
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, (mid - y0) % step, "the split point left the lattice");

            uint8_t visits[21] = {0};
            for (int y = y0; y < mid; y += step) {
                visits[(y - y0) / step]++;
            }
            for (int y = mid; y < y1; y += step) {
                visits[(y - y0) / step]++;
            }
            for (int i = 0; i < rows; i++) {
                char why[80];
                snprintf(why, sizeof why, "step %d rows %d row %d: covered %d times, not once", step, rows, i,
                         visits[i]);
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, visits[i], why);
            }
        }
    }
}

static void
test_split_mid_is_degenerate_at_zero_or_one_row(void) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(10, rt_refine_split_mid(10, 10, 4), "an empty range must split at its own edge");
    TEST_ASSERT_EQUAL_INT_MESSAGE(10, rt_refine_split_mid(10, 14, 4),
                                  "a one-row range must leave the first half empty");
}

/* Range-end: honours the budget, never traces less than it must */

static void
test_lattice_range_end_stops_as_soon_as_the_budget_is_met(void) {
    const int width = 37, height = 23, step = 4, budget = 30;
    const int end = rt_refine_lattice_range_end(width, height, 0, step, budget);

    int through_end = 0;
    for (int y = 0; y < end; y += step) {
        through_end += rt_refine_row_cost(width, y, step);
    }
    TEST_ASSERT_TRUE_MESSAGE(through_end >= budget || end >= height,
                             "the range ended before its budget or the picture were satisfied");

    if (end > 0) {
        int before_last_row = 0;
        for (int y = 0; y < end - step; y += step) {
            before_last_row += rt_refine_row_cost(width, y, step);
        }
        TEST_ASSERT_LESS_THAN_INT_MESSAGE(budget, before_last_row, "the range included a row the budget did not need");
    }
}

static void
test_lattice_range_end_always_advances_when_pixels_remain(void) {
    const int end = rt_refine_lattice_range_end(20, 20, 0, 8, 1);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, end, "a budget smaller than one row's cost must still trace that row");
}

static void
test_uniform_range_end_counts_whole_rows(void) {
    TEST_ASSERT_EQUAL_INT(3, rt_refine_uniform_range_end(100, 0, 10, 25));
    TEST_ASSERT_EQUAL_INT_MESSAGE(10, rt_refine_uniform_range_end(10, 0, 10, 1000), "the range must stop at height");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, rt_refine_uniform_range_end(10, 0, 10, 1), "the first row is never optional");
}

/* THE DECISIVE TEST: rt_cornell_render_rows(), split at every row */

#define DET_W 40
#define DET_H 30

static rt_cornell_camera_t
cornell_camera(void) {
    rt_cornell_camera_t cam;
    rt_cornell_camera_init(&cam, (r3d_viewport_t){DET_W, DET_H, 0});
    return cam;
}

static void
assert_cornell_split_matches_whole(int y0, int y1, int step, int mid) {
    const rt_cornell_camera_t cam = cornell_camera();
    const size_t bytes = sizeof(gfx_color_t) * (size_t)DET_W * DET_H;
    gfx_color_t* whole = malloc(bytes);
    gfx_color_t* split = malloc(bytes);
    TEST_ASSERT_NOT_NULL(whole);
    TEST_ASSERT_NOT_NULL(split);
    /* Same sentinel in both: a range this short leaves rows genuinely
     * untraced on both sides, and a differing fill there would read as a
     * mismatch that has nothing to do with the split. */
    memset(whole, 0xAA, bytes);
    memset(split, 0xAA, bytes);

    rt_cornell_render_rows(&cam, whole, y0, y1, step);
    rt_cornell_render_rows(&cam, split, y0, mid, step);
    rt_cornell_render_rows(&cam, split, mid, y1, step);

    char why[96];
    snprintf(why, sizeof why, "y0=%d y1=%d step=%d mid=%d: a split trace disagreed with the whole-range trace", y0, y1,
             step, mid);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(whole, split, bytes, why);

    free(whole);
    free(split);
}

/* Every row-aligned split of a ragged range, at the coarsest and the finest
 * lattice step - the two ends of the row-cost variance rt_refine_is_new()
 * produces, and both degenerate splits (k=0 empty first half, k=rows empty
 * second half) fall out of the loop's own endpoints. */
static void
assert_every_split_of_a_ragged_range_matches(int step) {
    const int y1 = DET_H - 2; /* not a multiple of step: a ragged edge */
    const int rows = y1 / step;

    for (int k = 0; k <= rows; k++) {
        assert_cornell_split_matches_whole(0, y1, step, k * step);
    }
}

static void
test_cornell_split_matches_whole_at_every_row_step_one(void) {
    assert_every_split_of_a_ragged_range_matches(1);
}

static void
test_cornell_split_matches_whole_at_every_row_step_eight(void) {
    assert_every_split_of_a_ragged_range_matches(RT_REFINE_FIRST_STEP);
}

static void
test_render_lattice_budget_matches_render_rows_over_the_same_range(void) {
    const rt_cornell_camera_t cam = cornell_camera();
    const size_t bytes = sizeof(gfx_color_t) * (size_t)DET_W * DET_H;
    gfx_color_t* dispatched = malloc(bytes);
    gfx_color_t* reference = malloc(bytes);
    TEST_ASSERT_NOT_NULL(dispatched);
    TEST_ASSERT_NOT_NULL(reference);
    memset(dispatched, 0xAA, bytes);
    memset(reference, 0xAA, bytes);

    const int end_y = rt_cornell_render_lattice_budget(&cam, dispatched, 0, RT_REFINE_FIRST_STEP, 40);
    rt_cornell_render_rows(&cam, reference, 0, end_y, RT_REFINE_FIRST_STEP);

    TEST_ASSERT_GREATER_THAN_INT(0, end_y);
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(DET_H + RT_REFINE_FIRST_STEP, end_y,
                                      "the range overshot the picture by more than one row");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(reference, dispatched, bytes,
                                     "the budgeted, core-split render disagreed with the plain reference tracer");

    free(dispatched);
    free(reference);
}

/* Same claim, the path tracer's seed pass: rt_path_seed_rows() */

#define PT_W 26
#define PT_H 19

static rt_cornell_camera_t
path_camera(void) {
    rt_cornell_camera_t cam;
    rt_cornell_camera_init(&cam, (r3d_viewport_t){PT_W, PT_H, 0});
    return cam;
}

static void
assert_seed_split_matches_whole(int y0, int y1, int step, int mid) {
    const rt_cornell_camera_t cam = path_camera();
    const size_t fb_bytes = sizeof(gfx_color_t) * (size_t)PT_W * PT_H;
    const size_t accum_bytes = sizeof(rt_path_accum_px_t) * (size_t)PT_W * PT_H;
    gfx_color_t* whole_fb = malloc(fb_bytes);
    gfx_color_t* split_fb = malloc(fb_bytes);
    rt_path_accum_px_t* whole_accum = calloc((size_t)PT_W * PT_H, sizeof(*whole_accum));
    rt_path_accum_px_t* split_accum = calloc((size_t)PT_W * PT_H, sizeof(*split_accum));
    TEST_ASSERT_NOT_NULL(whole_fb);
    TEST_ASSERT_NOT_NULL(split_fb);
    TEST_ASSERT_NOT_NULL(whole_accum);
    TEST_ASSERT_NOT_NULL(split_accum);
    memset(whole_fb, 0xAA, fb_bytes);
    memset(split_fb, 0xAA, fb_bytes);

    rt_path_seed_rows(&cam, (rt_path_target_t){whole_fb, whole_accum, PT_W, PT_H}, y0, y1, step);
    rt_path_seed_rows(&cam, (rt_path_target_t){split_fb, split_accum, PT_W, PT_H}, y0, mid, step);
    rt_path_seed_rows(&cam, (rt_path_target_t){split_fb, split_accum, PT_W, PT_H}, mid, y1, step);

    char why[96];
    snprintf(why, sizeof why, "y0=%d y1=%d step=%d mid=%d: a split seed pass disagreed with the whole-range one", y0,
             y1, step, mid);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(whole_fb, split_fb, fb_bytes, why);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(whole_accum, split_accum, accum_bytes, why);

    free(whole_fb);
    free(split_fb);
    free(whole_accum);
    free(split_accum);
}

static void
test_path_seed_split_matches_whole_at_every_row(void) {
    const int step = 2;
    const int y1 = PT_H - 1;
    const int rows = y1 / step;

    for (int k = 0; k <= rows; k++) {
        assert_seed_split_matches_whole(0, y1, step, k * step);
    }
}

/* Same claim, the path tracer's sweep pass: rt_path_sweep_rows() */

static void
assert_sweep_split_matches_whole(int y0, int y1, int mid, uint32_t n) {
    const rt_cornell_camera_t cam = path_camera();
    const size_t fb_bytes = sizeof(gfx_color_t) * (size_t)PT_W * PT_H;
    const size_t accum_bytes = sizeof(rt_path_accum_px_t) * (size_t)PT_W * PT_H;
    gfx_color_t* whole_fb = malloc(fb_bytes);
    gfx_color_t* split_fb = malloc(fb_bytes);
    rt_path_accum_px_t* whole_accum = malloc(accum_bytes);
    rt_path_accum_px_t* split_accum = malloc(accum_bytes);
    TEST_ASSERT_NOT_NULL(whole_fb);
    TEST_ASSERT_NOT_NULL(split_fb);
    TEST_ASSERT_NOT_NULL(whole_accum);
    TEST_ASSERT_NOT_NULL(split_accum);
    /* A nonzero starting mean, not a freshly seeded one: exercises the
     * running-mean blend rather than only its n==1 seeding case. */
    for (int i = 0; i < PT_W * PT_H; i++) {
        whole_accum[i] = split_accum[i] = (rt_path_accum_px_t){500, 500, 500};
    }
    memset(whole_fb, 0xAA, fb_bytes);
    memset(split_fb, 0xAA, fb_bytes);

    rt_path_sweep_rows(&cam, (rt_path_target_t){whole_fb, whole_accum, PT_W, PT_H}, y0, y1, n);
    rt_path_sweep_rows(&cam, (rt_path_target_t){split_fb, split_accum, PT_W, PT_H}, y0, mid, n);
    rt_path_sweep_rows(&cam, (rt_path_target_t){split_fb, split_accum, PT_W, PT_H}, mid, y1, n);

    char why[96];
    snprintf(why, sizeof why, "y0=%d y1=%d mid=%d n=%u: a split sweep pass disagreed with the whole-range one", y0, y1,
             mid, (unsigned)n);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(whole_fb, split_fb, fb_bytes, why);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(whole_accum, split_accum, accum_bytes, why);

    free(whole_fb);
    free(split_fb);
    free(whole_accum);
    free(split_accum);
}

static void
test_path_sweep_split_matches_whole_at_every_row(void) {
    const int y1 = PT_H - 3;

    for (int mid = 0; mid <= y1; mid++) {
        assert_sweep_split_matches_whole(0, y1, mid, 5);
    }
}

void
run_rt_row_split_suite(void) {
    RUN_TEST(test_split_mid_covers_every_row_exactly_once);
    RUN_TEST(test_split_mid_is_degenerate_at_zero_or_one_row);

    RUN_TEST(test_lattice_range_end_stops_as_soon_as_the_budget_is_met);
    RUN_TEST(test_lattice_range_end_always_advances_when_pixels_remain);
    RUN_TEST(test_uniform_range_end_counts_whole_rows);

    RUN_TEST(test_cornell_split_matches_whole_at_every_row_step_one);
    RUN_TEST(test_cornell_split_matches_whole_at_every_row_step_eight);
    RUN_TEST(test_render_lattice_budget_matches_render_rows_over_the_same_range);

    RUN_TEST(test_path_seed_split_matches_whole_at_every_row);
    RUN_TEST(test_path_sweep_split_matches_whole_at_every_row);
}

SUITE_REGISTER(run_rt_row_split_suite);

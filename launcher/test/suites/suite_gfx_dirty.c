/*
 * Portable suite: gfx_dirty - the grid/leaf dirty-region tracker.
 *
 * gfx_dirty.h carries no ESP-IDF dependency, unlike gfx.c (which
 * unconditionally includes ESP-IDF SPI headers) - that split is what
 * makes this logic reachable from a host at all.
 * suite_gfx.c (device-only) still covers whether the design is actually
 * cheaper to send; this suite covers whether the geometry and bitmask
 * logic underneath it is correct in the first place, which a timing
 * comparison on real hardware can never directly show. Off-by-one at a
 * LEAF_W/LEAF_H boundary is the likely bug class here, so several of these
 * lean on the exact boundary pixel rather than a value safely in the
 * middle of a leaf.
 */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_dirty.h"

/* gfx_dirty.h's state is static, so this file gets its own private copy -
 * exactly like a real device build's gfx.c does, and exactly what makes it
 * possible to reset and inspect directly here. */
static void
fixture(void) {
    cell_dirty = 0;
    all_dirty = false;
    for (int i = 0; i < CELL_COUNT; i++) {
        const int col = i % GRID_COLS;
        const int row = i / GRID_COLS;
        cell_x0[i] = (col + 1) * COL_WIDTH;
        cell_x1[i] = col * COL_WIDTH;
        cell_y0[i] = row * STRIP_HEIGHT + STRIP_HEIGHT;
        cell_y1[i] = row * STRIP_HEIGHT;
    }
    for (int i = 0; i < STRIP_COUNT * LEAF_SUB; i++) {
        leaf_dirty[i] = 0;
    }
}

/* leaf boundary math */

static void
test_mark_leaves_stays_in_the_leaf_before_a_boundary(void) {
    fixture();
    dirty_mark(LEAF_W - 1, 0, 1, 1); /* the last pixel of leaf column 0 */

    TEST_ASSERT_EQUAL_HEX16(0x0001, leaf_dirty[0]);
}

static void
test_mark_leaves_moves_to_the_next_leaf_at_a_boundary(void) {
    fixture();
    dirty_mark(LEAF_W, 0, 1, 1); /* the first pixel of leaf column 1 */

    TEST_ASSERT_EQUAL_HEX16(0x0002, leaf_dirty[0]);
}

static void
test_mark_leaves_sets_every_leaf_a_wide_box_spans(void) {
    fixture();
    /* Spans leaf columns 0, 1 and part of 2 (0..49, LEAF_W=23: cols
     * [0,23) [23,46) [46,69)). */
    dirty_mark(0, 0, 50, 1);

    TEST_ASSERT_EQUAL_HEX16(0x0007, leaf_dirty[0]);
}

static void
test_mark_leaves_spans_a_cell_boundary_too(void) {
    fixture();
    /* COL_WIDTH=92=LEAF_W*4, so a box straddling x=92 crosses from the
     * last leaf of cell 0 (column 3) into the first leaf of cell 1
     * (column 4) - a boundary between LEAF_SUB groups, not just between
     * two leaves within one. */
    dirty_mark(COL_WIDTH - 1, 0, 2, 1);

    TEST_ASSERT_EQUAL_HEX16(0x0018, leaf_dirty[0]); /* bits 3 and 4 */
}

/*
 * dirty_leaf_rects()
 *
 * Backs the leaf debug-overlay layer in gfx.c - one rectangle per dirty
 * leaf, unmerged, clipped to the caller's box. A host build cannot reach
 * gfx.c's overlay code at all (it is CONFIG_LAUNCHER_DEVELOPMENT, ESP-IDF-
 * dependent), so this enumerator, not the drawing that consumes it, is
 * where the geometry actually gets proven correct.
 */

static void
test_dirty_leaf_rects_finds_nothing_in_a_clean_row(void) {
    fixture();

    dirty_leaf_rect_t out[8];
    const int n = dirty_leaf_rects(0, 0, 0, GFX_DIRTY_WIDTH, STRIP_HEIGHT, out, 8);

    TEST_ASSERT_EQUAL_INT(0, n);
}

static void
test_dirty_leaf_rects_one_pixel_yields_the_whole_leaf(void) {
    fixture();
    dirty_mark(5, 5, 1, 1); /* leaf column 0, leaf row 0 */

    dirty_leaf_rect_t out[8];
    const int n = dirty_leaf_rects(0, 0, 0, GFX_DIRTY_WIDTH, STRIP_HEIGHT, out, 8);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(0, out[0].x0);
    TEST_ASSERT_EQUAL_INT(LEAF_W, out[0].x1);
    TEST_ASSERT_EQUAL_INT(0, out[0].y0);
    TEST_ASSERT_EQUAL_INT(LEAF_H, out[0].y1);
}

static void
test_dirty_leaf_rects_two_columns_are_two_adjacent_rects(void) {
    fixture();
    dirty_mark(LEAF_W - 1, 0, 2, 1); /* straddles leaf columns 0 and 1 */

    dirty_leaf_rect_t out[8];
    const int n = dirty_leaf_rects(0, 0, 0, GFX_DIRTY_WIDTH, STRIP_HEIGHT, out, 8);

    TEST_ASSERT_EQUAL_INT_MESSAGE(2, n,
                                  "one rect per dirty leaf, never merged into a run - merging would "
                                  "hide the leaf boundary this layer exists to show");
    TEST_ASSERT_EQUAL_INT(0, out[0].x0);
    TEST_ASSERT_EQUAL_INT(LEAF_W, out[0].x1);
    TEST_ASSERT_EQUAL_INT(LEAF_W, out[1].x0);
    TEST_ASSERT_EQUAL_INT(2 * LEAF_W, out[1].x1);
}

static void
test_dirty_leaf_rects_two_leaf_rows_both_appear(void) {
    fixture();
    dirty_mark(5, LEAF_H - 1, 1, 2); /* straddles leaf rows 0 and 1 */

    dirty_leaf_rect_t out[8];
    const int n = dirty_leaf_rects(0, 0, 0, GFX_DIRTY_WIDTH, STRIP_HEIGHT, out, 8);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, out[0].y0);
    TEST_ASSERT_EQUAL_INT(LEAF_H, out[0].y1);
    TEST_ASSERT_EQUAL_INT(LEAF_H, out[1].y0);
    TEST_ASSERT_EQUAL_INT(2 * LEAF_H, out[1].y1);
}

static void
test_dirty_leaf_rects_clips_to_the_callers_box(void) {
    fixture();
    dirty_mark(5, 5, 1, 1); /* leaf column 0, leaf row 0: full extent
                                 [0,LEAF_W) x [0,LEAF_H) */

    dirty_leaf_rect_t out[8];
    const int n = dirty_leaf_rects(0, 5, 3, 15, 10, out, 8);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, n,
                                  "the leaf is still one dirty leaf, just narrower than its own "
                                  "full extent once clipped");
    TEST_ASSERT_EQUAL_INT(5, out[0].x0);
    TEST_ASSERT_EQUAL_INT(15, out[0].x1);
    TEST_ASSERT_EQUAL_INT(3, out[0].y0);
    TEST_ASSERT_EQUAL_INT(10, out[0].y1);
}

static void
test_dirty_leaf_rects_a_leaf_entirely_outside_the_box_is_dropped(void) {
    fixture();
    dirty_mark(5, 5, 1, 1); /* leaf column 0: [0,LEAF_W) */

    dirty_leaf_rect_t out[8];
    const int n = dirty_leaf_rects(0, 30, 0, 50, STRIP_HEIGHT, out, 8);

    TEST_ASSERT_EQUAL_INT(0, n);
}

static void
test_dirty_leaf_rects_respects_the_max_out_cap(void) {
    fixture();
    leaf_dirty[0] = 0xFFFF; /* every leaf column dirty in leaf row 0 */

    dirty_leaf_rect_t out[16];
    const int n = dirty_leaf_rects(0, 0, 0, GFX_DIRTY_WIDTH, STRIP_HEIGHT, out, 5);

    TEST_ASSERT_EQUAL_INT(5, n);
}

static void
test_dirty_leaf_rects_leaf_rows_are_independent(void) {
    fixture();
    dirty_mark(5, 0, 1, 1);          /* leaf row 0 */
    dirty_mark(5, 2 * LEAF_H, 1, 1); /* leaf row 2 */

    /* Box only spans leaf row 0's own height - leaf row 2's rect must not
     * leak into this result just because it shares the same strip row. */
    dirty_leaf_rect_t out[8];
    const int n = dirty_leaf_rects(0, 0, 0, GFX_DIRTY_WIDTH, LEAF_H, out, 8);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(0, out[0].y0);
    TEST_ASSERT_EQUAL_INT(LEAF_H, out[0].y1);
}

/* eligibility */

static void
test_run_is_leaf_eligible_when_every_cell_is_tight(void) {
    fixture();
    dirty_mark(5, 0, 10, 10);
    dirty_mark(COL_WIDTH + 5, 0, 10, 10);

    TEST_ASSERT_TRUE(run_is_leaf_eligible(0, 0, 2));
}

static void
test_run_is_leaf_eligible_false_if_any_cell_in_the_run_is_coarse(void) {
    fixture();
    /* Cell 0's box exactly fills its own full extent - treated the same as
     * a mark_band() touch, per dirty_mark()'s own documented invariant. */
    dirty_mark(0, 0, COL_WIDTH, STRIP_HEIGHT);
    dirty_mark(COL_WIDTH + 5, 0, 10, 10);

    TEST_ASSERT_FALSE_MESSAGE(run_is_leaf_eligible(0, 0, 2),
                              "one coarse cell must disable refinement for the whole run, not "
                              "just that cell");
}

/*
 * collect_runs_from_mask
 *
 * Shared by the cell-level and leaf-level run finders in gfx.c - these
 * exercise it directly, at the bit-manipulation level, rather than only
 * indirectly through whichever caller happens to reach it.
 */

static void
test_collect_runs_from_mask_finds_nothing_in_an_empty_mask(void) {
    int s[4], e[4];

    TEST_ASSERT_EQUAL_INT(0, collect_runs_from_mask(0, 8, s, e, 4));
}

static void
test_collect_runs_from_mask_finds_one_contiguous_run(void) {
    int s[4], e[4];
    const int n = collect_runs_from_mask(0x3C /* 0b00111100 */, 8, s, e, 4);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(2, s[0]);
    TEST_ASSERT_EQUAL_INT(6, e[0]);
}

static void
test_collect_runs_from_mask_separates_a_real_gap(void) {
    int s[4], e[4];
    /* Bit 0 and bit 7 - opposite ends of the mask, nothing shared. */
    const int n = collect_runs_from_mask(0x81 /* 0b10000001 */, 8, s, e, 4);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, s[0]);
    TEST_ASSERT_EQUAL_INT(1, e[0]);
    TEST_ASSERT_EQUAL_INT(7, s[1]);
    TEST_ASSERT_EQUAL_INT(8, e[1]);
}

static void
test_collect_runs_from_mask_no_gap_is_one_run(void) {
    int s[4], e[4];
    const int n = collect_runs_from_mask(0xFF, 8, s, e, 4);

    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(0, s[0]);
    TEST_ASSERT_EQUAL_INT(8, e[0]);
}

static void
test_collect_runs_from_mask_fits_exactly_at_the_cap(void) {
    int s[2], e[2];
    /* Two isolated single-bit runs, cap of exactly 2. */
    const int n = collect_runs_from_mask(0x05 /* 0b00000101 */, 8, s, e, 2);

    TEST_ASSERT_EQUAL_INT(2, n);
}

static void
test_collect_runs_from_mask_gives_up_past_the_cap(void) {
    int s[2], e[2];
    /* Four isolated single-bit runs, cap of 2 - the third one must trip
     * the "too fragmented" case. */
    const int n = collect_runs_from_mask(0x55 /* 0b01010101 */, 8, s, e, 2);

    TEST_ASSERT_EQUAL_INT(-1, n);
}

/*
 * cell-level runs and boxes
 *
 * collect_dirty_runs()/run_box() are the cell-granularity counterparts of
 * collect_runs_from_mask() above, and plan_run() is what decides whether a
 * run gets leaf-refined at all - exercised directly here rather than only
 * indirectly through gfx.c's send path, which a host build cannot reach.
 */

static void
test_collect_dirty_runs_finds_two_separate_cell_runs(void) {
    fixture();
    dirty_mark(5, 0, 10, 10);                 /* cell 0 */
    dirty_mark(3 * COL_WIDTH + 5, 0, 10, 10); /* cell 3 - opposite end */

    int run_start[GRID_COLS], run_end[GRID_COLS];
    const int n = collect_dirty_runs(0, run_start, run_end);

    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_INT(0, run_start[0]);
    TEST_ASSERT_EQUAL_INT(1, run_end[0]);
    TEST_ASSERT_EQUAL_INT(3, run_start[1]);
    TEST_ASSERT_EQUAL_INT(4, run_end[1]);
}

static void
test_run_box_unions_every_cell_in_the_run(void) {
    fixture();
    dirty_mark(5, 10, 10, 10);            /* cell 0: x[5,15) y[10,20) */
    dirty_mark(COL_WIDTH + 20, 0, 5, 30); /* cell 1: x[+20,+25) y[0,30) */

    int x0, x1, y0, y1;
    run_box(0, 0, 2, &x0, &x1, &y0, &y1);

    TEST_ASSERT_EQUAL_INT(5, x0);
    TEST_ASSERT_EQUAL_INT(COL_WIDTH + 25, x1);
    TEST_ASSERT_EQUAL_INT(0, y0);
    TEST_ASSERT_EQUAL_INT(30, y1);
}

static void
test_plan_run_finds_a_real_gap_inside_one_cell(void) {
    fixture();
    /* Two marks in the same cell, far enough apart to leave a real leaf
     * gap between them - the exact shape test_two_marks_in_one_cell_costs_
     * less_than_the_coarse_box exercises on real hardware in suite_gfx.c;
     * this is the same case at the logic level, host-side. */
    dirty_mark(5, 0, 5, 5);
    dirty_mark(70, 0, 5, 5);

    int sx0[LEAF_REFINE_MAX_RUNS], sx1[LEAF_REFINE_MAX_RUNS];
    const int n = plan_run(0, 0, 1, 0, 5, sx0, sx1);

    TEST_ASSERT_EQUAL_INT(2, n);
}

/* Derived from GATHER_MAX_PIXELS/STRIP_HEIGHT plus a margin rather than
 * hardcoded, so the "over budget" case tracks whatever the budget is tuned
 * to. Valid only while two such marks, with a gap between them, still fit
 * inside one 4-cell run - the static assert below catches a
 * GATHER_MAX_PIXELS large enough to break that. */
#define OVER_BUDGET_MARK_W ((GATHER_MAX_PIXELS / STRIP_HEIGHT) + 8)

static void
test_plan_run_rejects_a_split_over_the_gather_budget(void) {
    _Static_assert(OVER_BUDGET_MARK_W < (GRID_COLS * COL_WIDTH) / 2 - 20,
                   "GATHER_MAX_PIXELS is too large for two over-budget marks to both "
                   "fit, with a real gap, inside one 4-cell run - this test's "
                   "construction needs rethinking above this budget, not a bigger "
                   "mark");
    fixture();
    /* Two wide marks at opposite ends of a 4-cell run, full strip height -
     * each half, once split, is over GATHER_MAX_PIXELS. Must fall back to
     * 0 (use the coarse box) rather than hand back a split
     * gather_and_send()'s fixed-size buffer cannot hold. */
    dirty_mark(1, 0, OVER_BUDGET_MARK_W, STRIP_HEIGHT);
    dirty_mark(GRID_COLS * COL_WIDTH - 1 - OVER_BUDGET_MARK_W, 0, OVER_BUDGET_MARK_W, STRIP_HEIGHT);

    int sx0[LEAF_REFINE_MAX_RUNS], sx1[LEAF_REFINE_MAX_RUNS];
    const int n = plan_run(0, 0, 4, 0, STRIP_HEIGHT, sx0, sx1);

    TEST_ASSERT_EQUAL_INT(0, n);
}

/* per-row reset, once a row has been sent */

static void
test_row_sent_clears_only_that_rows_leaves(void) {
    fixture();
    dirty_mark(0, 0, 10, 10);            /* row 0's leaves */
    dirty_mark(0, STRIP_HEIGHT, 10, 10); /* row 1's leaves */

    TEST_ASSERT_NOT_EQUAL(0, leaf_dirty[0]);
    TEST_ASSERT_NOT_EQUAL(0, leaf_dirty[LEAF_SUB]);

    dirty_row_sent(0);

    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0, leaf_dirty[0], "the row that was just sent must have its leaves cleared");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, leaf_dirty[LEAF_SUB],
                                  "a stale bit from a past frame would make a genuinely fully dirty "
                                  "cell look like it has a gap that is not real, silently dropping "
                                  "pixels that do need sending - a DIFFERENT row must be untouched");
}

static void
test_row_sent_resets_cell_boxes_to_empty(void) {
    fixture();
    dirty_mark(5, 0, 10, 10);

    dirty_row_sent(0);

    TEST_ASSERT_TRUE_MESSAGE(cell_x0[0] > cell_x1[0], "next frame's union has to start from nothing, not keep growing "
                                                      "the previous frame's box forever");
}

/* the all_dirty fast path */

static void
test_mark_all_claims_every_row(void) {
    fixture();
    dirty_mark_all();

    TEST_ASSERT_TRUE(dirty_row_is_dirty(0));
    TEST_ASSERT_TRUE(dirty_row_is_dirty(STRIP_COUNT - 1));
}

static void
test_dirty_mark_is_a_noop_once_everything_is_already_claimed(void) {
    fixture();
    dirty_mark_all();
    cell_x0[0] = 12345; /* a value dirty_mark() must not touch */

    dirty_mark(5, 5, 10, 10);

    TEST_ASSERT_EQUAL_INT_MESSAGE(12345, cell_x0[0],
                                  "once the whole screen is already claimed, a further tight mark "
                                  "can only ever repeat work already done - it must not run at all");
}

/*
 * dirty_band_extent(): band mode's own "does this row range need
 * touching" query
 */

static void
test_band_extent_false_when_nothing_is_dirty(void) {
    fixture();
    int x0, x1;
    TEST_ASSERT_FALSE(dirty_band_extent(0, 32, &x0, &x1));
}

/* An odd-edged mark rounds outward to even, the same rule gfx.c's own
 * even_floor()/even_ceil() apply to a real panel window. */
static void
test_band_extent_returns_the_marked_x_range_rounded_even(void) {
    fixture();
    dirty_mark(11, 5, 21, 10); /* x in [11, 32) */

    int x0, x1;
    TEST_ASSERT_TRUE(dirty_band_extent(0, 32, &x0, &x1));
    TEST_ASSERT_EQUAL_INT(10, x0); /* 11 rounded down */
    TEST_ASSERT_EQUAL_INT(32, x1); /* already even */
}

static void
test_band_extent_ignores_a_band_outside_the_marked_rows(void) {
    fixture();
    dirty_mark(10, 10, 20, 20); /* rows [10, 30) */

    int x0, x1;
    TEST_ASSERT_FALSE_MESSAGE(dirty_band_extent(200, 232, &x0, &x1),
                              "a band nowhere near the mark must report nothing dirty");
}

/* A band-ring band is often narrower than STRIP_HEIGHT (64) - the query
 * must use the mark's own narrowed cell_y0/y1, not the whole strip, so a
 * band above or below the real mark inside the SAME strip still misses it. */
static void
test_band_extent_only_sees_the_part_of_a_strip_its_own_range_covers(void) {
    fixture();
    dirty_mark(0, 40, 8, 4); /* rows [40, 44), well inside strip 0 (rows 0-63) */

    int x0, x1;
    TEST_ASSERT_TRUE_MESSAGE(dirty_band_extent(32, 64, &x0, &x1), "a band containing the mark's real rows must see it");
    TEST_ASSERT_FALSE_MESSAGE(dirty_band_extent(0, 32, &x0, &x1),
                              "a band in the same strip but above the mark's own rows must not");
}

/* dirty_mark_all() is what a caller reaches for to force everything - band
 * mode's own "orientation change / app enter / gfx_invalidate()" case (see
 * gfx.c) ends up here too, so this is the query-side half of that promise:
 * once marked, every row range reports the full width dirty. */
static void
test_band_extent_is_full_width_once_everything_is_marked(void) {
    fixture();
    dirty_mark_all();

    int x0, x1;
    TEST_ASSERT_TRUE(dirty_band_extent(0, 32, &x0, &x1));
    TEST_ASSERT_EQUAL_INT(0, x0);
    TEST_ASSERT_EQUAL_INT(GFX_DIRTY_WIDTH, x1);
    TEST_ASSERT_TRUE(dirty_band_extent(416, 448, &x0, &x1));
}

/* A moving box across several frames, marked and queried the way a band-mode
 * frame loop repeats it: mark old+new bounds, query every band, then
 * dirty_frame_sent(). A caller that stops marking after frame one shows every
 * band skipped from frame two on, and only a multi-frame sequence sees it. */
static void
test_band_extent_follows_a_moving_box_across_several_frames(void) {
    fixture();

    /* One full STRIP_HEIGHT per frame: a smaller step re-marks the same
     * cell with a different sub-range each frame, and a cell's box only
     * ever widens within its own lifetime (never narrows before the next
     * dirty_mark_all()) - correct, but it would make this test's exact
     * per-frame equality assert fail on a real, harmless over-touch. */
    const int band_height = 32; /* a real GFX_BAND_HEIGHT choice (gfx.h) - not reachable from here, see file comment */
    const int box_x = 10, box_w = 15, box_h = 20;
    int box_y = 0;
    bool prev_valid = false;
    int prev_y = 0;

    for (int frame = 0; frame < 4; frame++) {
        if (prev_valid) {
            dirty_mark(box_x, prev_y, box_w, box_h);
        }
        dirty_mark(box_x, box_y, box_w, box_h);

        for (int band = 0; band * band_height < GFX_DIRTY_HEIGHT; band++) {
            const int row0 = band * band_height;
            const int row1 = row0 + band_height;
            int x0, x1;
            const bool touched = dirty_band_extent(row0, row1, &x0, &x1);
            const bool overlaps_old = prev_valid && prev_y < row1 && prev_y + box_h > row0;
            const bool overlaps_new = box_y < row1 && box_y + box_h > row0;

            TEST_ASSERT_EQUAL_INT_MESSAGE(overlaps_old || overlaps_new, touched,
                                          "band touch/skip must follow the moving box every frame, not just the first");
        }

        dirty_frame_sent();
        prev_valid = true;
        prev_y = box_y;
        box_y += STRIP_HEIGHT;
    }
}

static void
test_region_query_skips_a_box_beside_dirt_in_the_same_rows(void) {
    fixture();
    dirty_mark(5, 5, 10, 10);

    TEST_ASSERT_FALSE(dirty_region_dirty(2 * COL_WIDTH, 5, 10, 10));
    TEST_ASSERT_FALSE(dirty_region_dirty(2 * LEAF_W, 5, 10, 10));
}

static void
test_region_query_finds_an_overlapping_box(void) {
    fixture();
    dirty_mark(5, 5, 10, 10);

    TEST_ASSERT_TRUE(dirty_region_dirty(10, 10, 10, 10));
}

static void
test_region_query_handles_strip_and_cell_boundaries(void) {
    fixture();
    dirty_mark(COL_WIDTH - 1, STRIP_HEIGHT - 1, 2, 2);

    TEST_ASSERT_TRUE(dirty_region_dirty(COL_WIDTH, STRIP_HEIGHT, 1, 1));
    TEST_ASSERT_TRUE(dirty_region_dirty(COL_WIDTH - 1, STRIP_HEIGHT - 1, 1, 1));
    TEST_ASSERT_FALSE(dirty_region_dirty(COL_WIDTH + LEAF_W, STRIP_HEIGHT, 1, 1));
}

static void
test_region_query_keeps_full_width_band_marks(void) {
    fixture();
    mark_band(STRIP_HEIGHT, 2 * STRIP_HEIGHT);

    TEST_ASSERT_TRUE(dirty_region_dirty(0, STRIP_HEIGHT + 5, 1, 1));
    TEST_ASSERT_TRUE(dirty_region_dirty(GFX_DIRTY_WIDTH - 1, STRIP_HEIGHT + 5, 1, 1));
    TEST_ASSERT_FALSE(dirty_region_dirty(0, STRIP_HEIGHT - 1, 1, 1));
}

static void
test_region_query_uses_leaf_gaps_within_one_cell(void) {
    fixture();
    dirty_mark(1, 5, 2, 2);
    dirty_mark(3 * LEAF_W, 5, 2, 2);

    TEST_ASSERT_FALSE(dirty_region_dirty(LEAF_W + 1, 5, 1, 1));
    TEST_ASSERT_TRUE(dirty_region_dirty(3 * LEAF_W, 5, 1, 1));
}

void
run_gfx_dirty_suite(void) {
    RUN_TEST(test_region_query_skips_a_box_beside_dirt_in_the_same_rows);
    RUN_TEST(test_region_query_finds_an_overlapping_box);
    RUN_TEST(test_region_query_handles_strip_and_cell_boundaries);
    RUN_TEST(test_region_query_keeps_full_width_band_marks);
    RUN_TEST(test_region_query_uses_leaf_gaps_within_one_cell);
    RUN_TEST(test_mark_leaves_stays_in_the_leaf_before_a_boundary);
    RUN_TEST(test_mark_leaves_moves_to_the_next_leaf_at_a_boundary);
    RUN_TEST(test_mark_leaves_sets_every_leaf_a_wide_box_spans);
    RUN_TEST(test_mark_leaves_spans_a_cell_boundary_too);

    RUN_TEST(test_dirty_leaf_rects_finds_nothing_in_a_clean_row);
    RUN_TEST(test_dirty_leaf_rects_one_pixel_yields_the_whole_leaf);
    RUN_TEST(test_dirty_leaf_rects_two_columns_are_two_adjacent_rects);
    RUN_TEST(test_dirty_leaf_rects_two_leaf_rows_both_appear);
    RUN_TEST(test_dirty_leaf_rects_clips_to_the_callers_box);
    RUN_TEST(test_dirty_leaf_rects_a_leaf_entirely_outside_the_box_is_dropped);
    RUN_TEST(test_dirty_leaf_rects_respects_the_max_out_cap);
    RUN_TEST(test_dirty_leaf_rects_leaf_rows_are_independent);

    RUN_TEST(test_run_is_leaf_eligible_when_every_cell_is_tight);
    RUN_TEST(test_run_is_leaf_eligible_false_if_any_cell_in_the_run_is_coarse);

    RUN_TEST(test_collect_runs_from_mask_finds_nothing_in_an_empty_mask);
    RUN_TEST(test_collect_runs_from_mask_finds_one_contiguous_run);
    RUN_TEST(test_collect_runs_from_mask_separates_a_real_gap);
    RUN_TEST(test_collect_runs_from_mask_no_gap_is_one_run);
    RUN_TEST(test_collect_runs_from_mask_fits_exactly_at_the_cap);
    RUN_TEST(test_collect_runs_from_mask_gives_up_past_the_cap);

    RUN_TEST(test_collect_dirty_runs_finds_two_separate_cell_runs);
    RUN_TEST(test_run_box_unions_every_cell_in_the_run);
    RUN_TEST(test_plan_run_finds_a_real_gap_inside_one_cell);
    RUN_TEST(test_plan_run_rejects_a_split_over_the_gather_budget);

    RUN_TEST(test_row_sent_clears_only_that_rows_leaves);
    RUN_TEST(test_row_sent_resets_cell_boxes_to_empty);

    RUN_TEST(test_mark_all_claims_every_row);
    RUN_TEST(test_dirty_mark_is_a_noop_once_everything_is_already_claimed);

    RUN_TEST(test_band_extent_false_when_nothing_is_dirty);
    RUN_TEST(test_band_extent_returns_the_marked_x_range_rounded_even);
    RUN_TEST(test_band_extent_ignores_a_band_outside_the_marked_rows);
    RUN_TEST(test_band_extent_only_sees_the_part_of_a_strip_its_own_range_covers);
    RUN_TEST(test_band_extent_is_full_width_once_everything_is_marked);
    RUN_TEST(test_band_extent_follows_a_moving_box_across_several_frames);
}

SUITE_REGISTER(run_gfx_dirty_suite);

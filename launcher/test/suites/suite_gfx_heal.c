/*
 * Portable suite: gfx_heal.h, the queue and strip planner behind
 * gfx_heal_mark(). The sends themselves need the panel and are not covered.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_heal.h"

#define WIDTH    368
#define STRIP_PX (WIDTH * GFX_HEAL_STRIP_ROWS)
#define ROOMY    (WIDTH * GFX_HEAL_SCREEN_ROWS)

static gfx_heal_t heal;
static gfx_heal_strip_t strips[GFX_HEAL_MAX_STRIPS];

static void
fixture(void) {
    gfx_heal_reset(&heal);
}

static int
plan(int budget) {
    return gfx_heal_plan(&heal, budget, WIDTH, strips, GFX_HEAL_MAX_STRIPS);
}

static bool
strips_cover(int n, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        bool covered = false;
        for (int i = 0; i < n; i++) {
            covered |= y >= strips[i].y0 && y < strips[i].y1;
        }
        if (!covered) {
            return false;
        }
    }
    return true;
}

static void
test_nothing_queued_plans_nothing(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(0, plan(ROOMY));
}

static void
test_queued_rows_come_back_as_strips_covering_them_then_are_forgotten(void) {
    fixture();
    gfx_heal_queue_rows(&heal, 100, 140);

    const int n = plan(ROOMY);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_TRUE_MESSAGE(strips_cover(n, 100, 140), "every queued row must be inside a strip");
    TEST_ASSERT_FALSE(gfx_heal_pending(&heal));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, plan(ROOMY), "a healed region is not sent again");
}

static void
test_strips_are_even_edged_and_on_screen(void) {
    fixture();
    gfx_heal_queue_rows(&heal, -20, GFX_HEAL_SCREEN_ROWS + 20);

    for (int round = 0; round < 4 && gfx_heal_pending(&heal); round++) {
        const int n = plan(ROOMY);
        for (int i = 0; i < n; i++) {
            TEST_ASSERT_TRUE(strips[i].y0 >= 0 && strips[i].y1 <= GFX_HEAL_SCREEN_ROWS);
            TEST_ASSERT_TRUE(strips[i].y0 < strips[i].y1);
            TEST_ASSERT_LESS_OR_EQUAL_INT(GFX_HEAL_STRIP_ROWS, strips[i].y1 - strips[i].y0);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, strips[i].y0 % 2, "the panel takes windows on even edges");
            TEST_ASSERT_EQUAL_INT(0, strips[i].y1 % 2);
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(gfx_heal_pending(&heal), "a whole-screen mark drains within a few presents");
}

static void
test_the_budget_bounds_a_present_and_the_rest_waits(void) {
    fixture();
    gfx_heal_queue_rows(&heal, 0, 64);
    gfx_heal_queue_rows(&heal, 300, 364);

    int n = plan(STRIP_PX);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_TRUE(gfx_heal_pending(&heal));

    int presents = 1;
    while (gfx_heal_pending(&heal) && presents < 16) {
        n = plan(STRIP_PX);
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(1, n, "one strip's budget sends at most one strip");
        presents++;
    }
    TEST_ASSERT_FALSE(gfx_heal_pending(&heal));
    TEST_ASSERT_LESS_OR_EQUAL_INT(8, presents);
}

static void
test_a_budget_below_one_strip_sends_nothing(void) {
    fixture();
    gfx_heal_queue_rows(&heal, 0, 8);
    TEST_ASSERT_EQUAL_INT(0, plan(STRIP_PX - 1));
    TEST_ASSERT_TRUE(gfx_heal_pending(&heal));
}

/* The reason a heal exists: the same rows healed twice are cut by strips
 * with different edges, so a shape that corrupted once is not repeated. */
static void
test_the_same_rows_healed_again_are_cut_differently(void) {
    fixture();
    gfx_heal_queue_rows(&heal, 200, 208);
    TEST_ASSERT_EQUAL_INT(1, plan(ROOMY));
    const gfx_heal_strip_t first = strips[0];

    gfx_heal_queue_rows(&heal, 200, 208);
    TEST_ASSERT_EQUAL_INT(1, plan(ROOMY));
    TEST_ASSERT_TRUE_MESSAGE(strips[0].y0 != first.y0, "the strip lattice must move between heals");
    TEST_ASSERT_TRUE(strips_cover(1, 200, 208));
}

static void
test_rolling_reaches_every_row_and_wraps(void) {
    fixture();
    static uint8_t covered[GFX_HEAL_SCREEN_ROWS];
    memset(covered, 0, sizeof covered);

    const int slice = GFX_HEAL_STRIP_ROWS;
    const int presents = GFX_HEAL_SCREEN_ROWS / slice + 2;
    for (int present = 0; present < presents; present++) {
        gfx_heal_queue_rolling(&heal, slice);
        const int n = plan(STRIP_PX * 2);
        for (int i = 0; i < n; i++) {
            for (int y = strips[i].y0; y < strips[i].y1; y++) {
                covered[y] = 1;
            }
        }
    }
    for (int y = 0; y < GFX_HEAL_SCREEN_ROWS; y++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, covered[y], "a rolling heal must reach every row");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE((presents * slice) % GFX_HEAL_SCREEN_ROWS, heal.rolling_row,
                                  "the sweep wraps back to the top");
}

static void
test_no_strips_overlap_nothing(void) {
    TEST_ASSERT_FALSE(gfx_heal_strips_overlap(strips, 0, 0, GFX_HEAL_SCREEN_ROWS));
}

static void
test_a_band_overlapping_a_planned_strip_is_reported(void) {
    fixture();
    gfx_heal_queue_rows(&heal, 100, 108);
    const int n = plan(ROOMY);

    TEST_ASSERT_TRUE_MESSAGE(gfx_heal_strips_overlap(strips, n, 64, 128), "the band [64, 128) contains row 100");
    TEST_ASSERT_FALSE_MESSAGE(gfx_heal_strips_overlap(strips, n, 128, 192), "the next band touches none of it");
}

static void
test_a_band_exactly_beside_a_strip_does_not_overlap(void) {
    gfx_heal_strip_t adjacent[1] = {{.y0 = 64, .y1 = 96}};
    TEST_ASSERT_FALSE_MESSAGE(gfx_heal_strips_overlap(adjacent, 1, 96, 128), "touching edges is not overlapping");
    TEST_ASSERT_TRUE(gfx_heal_strips_overlap(adjacent, 1, 32, 65));
}

/* PHASE_STEP=24 against STRIP_ROWS=32 cycles through exactly four values
 * before repeating - the concrete sequence a moving split point relies on. */
static void
test_advance_phase_cycles_through_four_values_then_repeats(void) {
    gfx_heal_t h;
    gfx_heal_reset(&h);

    const int expect[4] = {24, 16, 8, 0};
    for (int i = 0; i < 4; i++) {
        gfx_heal_advance_phase(&h);
        TEST_ASSERT_EQUAL_INT(expect[i], h.phase);
    }
    gfx_heal_advance_phase(&h);
    TEST_ASSERT_EQUAL_INT(expect[0], h.phase);
}

static void
test_band_split_row_stays_inside_the_band_and_even(void) {
    const int band_heights[3] = {16, 32, 64};
    const int phases[4] = {0, 8, 16, 24};

    for (int b = 0; b < 3; b++) {
        for (int p = 0; p < 4; p++) {
            const int row = gfx_heal_band_split_row(phases[p], band_heights[b]);
            TEST_ASSERT_TRUE(row >= 0 && row < band_heights[b]);
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, row % 2, "a split row must land on an even panel edge");
        }
    }
}

static void
test_band_split_row_worked_example(void) {
    TEST_ASSERT_EQUAL_INT(8, gfx_heal_band_split_row(24, 16));
    TEST_ASSERT_EQUAL_INT(24, gfx_heal_band_split_row(24, 32));
    TEST_ASSERT_EQUAL_INT(0, gfx_heal_band_split_row(32, 32));
}

void
run_gfx_heal_suite(void) {
    RUN_TEST(test_nothing_queued_plans_nothing);
    RUN_TEST(test_queued_rows_come_back_as_strips_covering_them_then_are_forgotten);
    RUN_TEST(test_strips_are_even_edged_and_on_screen);
    RUN_TEST(test_the_budget_bounds_a_present_and_the_rest_waits);
    RUN_TEST(test_a_budget_below_one_strip_sends_nothing);
    RUN_TEST(test_the_same_rows_healed_again_are_cut_differently);
    RUN_TEST(test_rolling_reaches_every_row_and_wraps);
    RUN_TEST(test_no_strips_overlap_nothing);
    RUN_TEST(test_a_band_overlapping_a_planned_strip_is_reported);
    RUN_TEST(test_a_band_exactly_beside_a_strip_does_not_overlap);
    RUN_TEST(test_advance_phase_cycles_through_four_values_then_repeats);
    RUN_TEST(test_band_split_row_stays_inside_the_band_and_even);
    RUN_TEST(test_band_split_row_worked_example);
}

SUITE_REGISTER(run_gfx_heal_suite);

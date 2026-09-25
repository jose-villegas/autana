/*
 * Portable suite: the sand renderer's per-cell decisions in sand_paint.h -
 * which neighbours count as an edge, how a liquid cell's local-depth count
 * climbs, restarts or holds, and how much of a row a send may cover. These
 * are the functions app_sand.c's paint loop inlines, called directly.
 */

#include <stdint.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "apps/sand/material.h"
#include "apps/sand/material_palette.h"
#include "apps/sand/sand_paint.h"

#define ROW_W 5

static const cell_t SAND = CELL_MAKE(MAT_SAND, 0);
static const cell_t WATER = CELL_MAKE(MAT_WATER, 0);
static const cell_t STONE = CELL_MAKE(MAT_STONE, 0);

static void
fill(cell_t* row, cell_t c) {
    for (int i = 0; i < ROW_W; i++) {
        row[i] = c;
    }
}

static void
test_paint_edge_mask_flags_each_empty_side(void) {
    cell_t above[ROW_W], row[ROW_W], below[ROW_W];
    fill(above, STONE);
    fill(row, STONE);
    fill(below, STONE);
    row[2] = SAND;

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, sand_paint_edge_mask(above, row, below, 2, ROW_W),
                                   "a cell with every neighbour occupied has no edge");

    row[1] = CELL_EMPTY;
    TEST_ASSERT_EQUAL_UINT(MATERIAL_EDGE_LEFT, sand_paint_edge_mask(above, row, below, 2, ROW_W));
    row[1] = STONE;
    row[3] = CELL_EMPTY;
    TEST_ASSERT_EQUAL_UINT(MATERIAL_EDGE_RIGHT, sand_paint_edge_mask(above, row, below, 2, ROW_W));
    row[3] = STONE;
    above[2] = CELL_EMPTY;
    TEST_ASSERT_EQUAL_UINT(MATERIAL_EDGE_UP, sand_paint_edge_mask(above, row, below, 2, ROW_W));
    above[2] = STONE;
    below[2] = CELL_EMPTY;
    TEST_ASSERT_EQUAL_UINT(MATERIAL_EDGE_DOWN, sand_paint_edge_mask(above, row, below, 2, ROW_W));
}

static void
test_paint_edge_mask_treats_the_grid_border_as_occupied(void) {
    cell_t row[ROW_W];
    fill(row, SAND);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, sand_paint_edge_mask(NULL, row, NULL, 0, ROW_W),
                                   "past the left edge, above the top row and below the bottom row "
                                   "there is no cell, and no cell is not an empty one");
    TEST_ASSERT_EQUAL_UINT(0u, sand_paint_edge_mask(NULL, row, NULL, ROW_W - 1, ROW_W));

    row[1] = CELL_EMPTY;
    TEST_ASSERT_EQUAL_UINT_MESSAGE(MATERIAL_EDGE_RIGHT, sand_paint_edge_mask(NULL, row, NULL, 0, ROW_W),
                                   "the border suppresses only its own side");
}

static void
test_paint_edge_mask_reads_diagonals_only_for_exposed_water(void) {
    cell_t above[ROW_W], row[ROW_W], below[ROW_W];
    fill(above, STONE);
    fill(row, STONE);
    fill(below, STONE);
    above[1] = CELL_EMPTY;
    below[3] = CELL_EMPTY;
    row[2] = WATER;

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, sand_paint_edge_mask(above, row, below, 2, ROW_W),
                                   "water walled in on all four sides ignores its empty diagonals");

    row[1] = CELL_EMPTY;
    TEST_ASSERT_EQUAL_UINT_MESSAGE(MATERIAL_EDGE_LEFT | MATERIAL_EDGE_UP_LEFT | MATERIAL_EDGE_DOWN_RIGHT,
                                   sand_paint_edge_mask(above, row, below, 2, ROW_W),
                                   "water with an empty side also reads its diagonals");

    row[2] = SAND;
    TEST_ASSERT_EQUAL_UINT_MESSAGE(MATERIAL_EDGE_LEFT, sand_paint_edge_mask(above, row, below, 2, ROW_W),
                                   "only water reads diagonals, exposed or not");
}

static void
test_paint_depth_count_climbs_from_the_same_material_up_to_the_ceiling(void) {
    uint8_t top_row[ROW_W];
    memset(top_row, 255, sizeof top_row);

    TEST_ASSERT_EQUAL_UINT(4u, sand_paint_depth_count(top_row, true, true, false, 3u, 1, 7));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(4u, sand_paint_depth_count(top_row, false, true, false, 3u, 1, 7),
                                   "a same-material climb carries the source count in either "
                                   "regime, whether or not the row chain held");
    TEST_ASSERT_EQUAL_UINT(LOCAL_DEPTH_COUNT_CEILING,
                           sand_paint_depth_count(top_row, true, true, true, LOCAL_DEPTH_COUNT_CEILING - 1u, 1, 7));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(LOCAL_DEPTH_COUNT_CEILING,
                                   sand_paint_depth_count(top_row, true, true, true, LOCAL_DEPTH_COUNT_CEILING, 1, 7),
                                   "the count saturates at the ceiling instead of wrapping");
}

static void
test_paint_depth_count_restart_carries_only_while_the_chain_holds(void) {
    uint8_t top_row[ROW_W];

    memset(top_row, 255, sizeof top_row);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(6u, sand_paint_depth_count(top_row, true, false, true, 5u, 2, 9),
                                   "a boundary cell whose row chain held carries the source count");
    memset(top_row, 255, sizeof top_row);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1u, sand_paint_depth_count(top_row, true, false, false, 5u, 2, 9),
                                   "a broken row chain restarts the count at one, whatever the "
                                   "stale source says");

    memset(top_row, 255, sizeof top_row);
    TEST_ASSERT_EQUAL_UINT(6u, sand_paint_depth_count(top_row, false, false, true, 5u, 2, 9));
    memset(top_row, 255, sizeof top_row);
    TEST_ASSERT_EQUAL_UINT(1u, sand_paint_depth_count(top_row, false, false, false, 5u, 2, 9));
}

static void
test_paint_depth_count_holds_a_column_that_already_committed(void) {
    uint8_t top_row[ROW_W];

    memset(top_row, 255, sizeof top_row);
    TEST_ASSERT_EQUAL_UINT(6u, sand_paint_depth_count(top_row, true, false, true, 5u, 2, 9));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(9u, top_row[2], "vertical-dominant commits by recording the row");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, sand_paint_depth_count(top_row, true, false, true, 5u, 2, 9),
                                   "the same row reaching the same column again reads as committed");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(6u, sand_paint_depth_count(top_row, true, false, true, 5u, 2, 10),
                                   "a different row is not held by another row's commit");

    memset(top_row, 255, sizeof top_row);
    TEST_ASSERT_EQUAL_UINT(6u, sand_paint_depth_count(top_row, false, false, true, 5u, 2, 9));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, top_row[2], "horizontal-dominant commits by marking the column 0");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, sand_paint_depth_count(top_row, false, false, true, 5u, 2, 10),
                                   "horizontal-dominant holds the column for any row");

    TEST_ASSERT_EQUAL_UINT(6u, sand_paint_depth_count(top_row, false, true, false, 5u, 2, 10));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(255u, top_row[2], "a horizontal-dominant same-material climb releases the column");
    TEST_ASSERT_EQUAL_UINT(6u, sand_paint_depth_count(top_row, false, false, true, 5u, 2, 11));
}

static void
test_paint_clip_send_keeps_to_the_painted_span(void) {
    int x0, x1;

    TEST_ASSERT_TRUE(sand_paint_clip_send(2, 12, 4, 9, false, 0, 0, &x0, &x1));
    TEST_ASSERT_EQUAL_INT(4, x0);
    TEST_ASSERT_EQUAL_INT(9, x1);

    TEST_ASSERT_TRUE(sand_paint_clip_send(5, 7, 4, 9, false, 0, 0, &x0, &x1));
    TEST_ASSERT_EQUAL_INT(5, x0);
    TEST_ASSERT_EQUAL_INT(7, x1);

    TEST_ASSERT_FALSE_MESSAGE(sand_paint_clip_send(2, 4, 4, 9, false, 0, 0, &x0, &x1),
                              "a range ending where the painted span starts sends nothing");
    TEST_ASSERT_FALSE(sand_paint_clip_send(9, 12, 4, 9, false, 0, 0, &x0, &x1));
}

static void
test_paint_clip_send_narrows_indexed_sends_to_what_changed(void) {
    int x0, x1;

    TEST_ASSERT_TRUE(sand_paint_clip_send(2, 12, 4, 9, true, 5, 7, &x0, &x1));
    TEST_ASSERT_EQUAL_INT(5, x0);
    TEST_ASSERT_EQUAL_INT(7, x1);

    TEST_ASSERT_FALSE_MESSAGE(sand_paint_clip_send(2, 12, 4, 9, true, 9, 0, &x0, &x1),
                              "an indexed row where no cell changed sends nothing");

    TEST_ASSERT_TRUE_MESSAGE(sand_paint_clip_send(2, 12, 4, 9, false, 9, 0, &x0, &x1),
                             "the RGB565 path ignores the changed span");
    TEST_ASSERT_EQUAL_INT(4, x0);
    TEST_ASSERT_EQUAL_INT(9, x1);
}

void
run_sand_paint_suite(void) {
    RUN_TEST(test_paint_edge_mask_flags_each_empty_side);
    RUN_TEST(test_paint_edge_mask_treats_the_grid_border_as_occupied);
    RUN_TEST(test_paint_edge_mask_reads_diagonals_only_for_exposed_water);
    RUN_TEST(test_paint_depth_count_climbs_from_the_same_material_up_to_the_ceiling);
    RUN_TEST(test_paint_depth_count_restart_carries_only_while_the_chain_holds);
    RUN_TEST(test_paint_depth_count_holds_a_column_that_already_committed);
    RUN_TEST(test_paint_clip_send_keeps_to_the_painted_span);
    RUN_TEST(test_paint_clip_send_narrows_indexed_sends_to_what_changed);
}

SUITE_REGISTER(run_sand_paint_suite);

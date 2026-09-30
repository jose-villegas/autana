/*
 * Portable suite: the gas spread pass's fast paths for a ray across the rows
 * (sideways gravity) leave every grid byte where the slow path puts it.
 *
 * The fingerprint's landscape rows are portrait scenes with gravity swapped,
 * so these boards are built for landscape: packed columns long enough for a
 * run to carry, holes for rays to find, blockers and a second gas to break
 * runs, rows packed from the edge the rays leave by, and fire to set it all
 * moving. Each is stepped twice from one seed, fast paths on and off
 * (sand_gas_line_fast_paths_enable(), sand_priv.h), and compared every step.
 */
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "apps/sand/sand.h"
#include "apps/sand/sand_priv.h"
#include "apps/sand/tests/suite_sand_scenes.h"

#define GASLINE_W     40
#define GASLINE_H     72
#define GASLINE_STEPS 40

typedef void (*gas_line_scene_fn)(sand_t* s);

typedef struct {
    uint8_t* cells;
    uint8_t* blocks;
    sand_t s;
} gas_line_board_t;

static gas_line_board_t fast_board;
static gas_line_board_t slow_board;

static void
gas_line_board_open(gas_line_board_t* b, gas_line_scene_fn scene, uint32_t seed) {
    const size_t blocks =
        (size_t)((GASLINE_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * ((GASLINE_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H);
    b->cells = malloc((size_t)GASLINE_W * (size_t)GASLINE_H);
    b->blocks = malloc(blocks);
    TEST_ASSERT_NOT_NULL(b->cells);
    TEST_ASSERT_NOT_NULL(b->blocks);
    sand_init(&b->s, b->cells, GASLINE_W, GASLINE_H, seed);
    sand_clear(&b->s);
    sand_enable_sleeping(&b->s, b->blocks);
    scene(&b->s);
}

static void
gas_lines_fixture(void) {
    memset(&fast_board, 0, sizeof fast_board);
    memset(&slow_board, 0, sizeof slow_board);
}

static void
gas_lines_teardown(void) {
    sand_gas_line_fast_paths_enable(true);
    free(fast_board.cells);
    free(fast_board.blocks);
    free(slow_board.cells);
    free(slow_board.blocks);
}

/* A cheap per-cell hash, so the holes and blockers fall differently in every
 * column rather than in a pattern a run could line up with. */
static unsigned
gas_line_hash(int x, int y) {
    unsigned h = ((unsigned)x * 73856093U) ^ ((unsigned)y * 19349663U);
    h ^= h >> 13;
    return h * 0x5bd1e995U;
}

/* Gas everywhere, with one hole in ~11 cells, a stone in ~37 and smoke in a
 * band, and the first and last rows left whole so a sweep starts packed. */
static void
scene_pocketed_gas(sand_t* s) {
    for (int y = 0; y < GASLINE_H; y++) {
        for (int x = 0; x < GASLINE_W; x++) {
            const unsigned h = gas_line_hash(x, y) >> 8;
            const bool edge_row = y < 6 || y >= GASLINE_H - 6;
            cell_t c = CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1);
            if (!edge_row && h % 11U == 0U) {
                c = CELL_EMPTY;
            } else if (!edge_row && h % 37U == 1U) {
                c = CELL_MAKE(MAT_STONE, 0);
            } else if (x >= GASLINE_W / 3 && x < GASLINE_W / 2) {
                c = CELL_MAKE(MAT_SMOKE, 8);
            }
            sand_set(s, x, y, c);
        }
    }
}

/* Smoke and steam in columns of alternating length, a quarter of the board
 * open, and fire at one corner to burn through the gas. */
static void
scene_burning_columns(sand_t* s) {
    for (int y = 0; y < GASLINE_H; y++) {
        for (int x = 0; x < GASLINE_W; x++) {
            const int span = (x & 1) ? GASLINE_H - 1 : (GASLINE_H * 3) / 4;
            cell_t c = CELL_EMPTY;
            if (x < (GASLINE_W * 3) / 4 && y < span) {
                c = ((x / 3 + y / 29) & 1) ? CELL_MAKE(MAT_STEAM, 8) : CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1);
            }
            sand_set(s, x, y, c);
        }
    }
    sand_set(s, 0, 0, CELL_MAKE(MAT_FIRE, 8));
}

/* Holes a column apart in how far they sit, so one column's scan finds
 * nothing within sight while its neighbour's finds a hole: a run carried into
 * the wrong column parts the grids. A row of holes above a packed band gives
 * a packed row that must still spread, which a skip too eager misses. */
static void
scene_staggered_holes(sand_t* s) {
    for (int y = 0; y < GASLINE_H; y++) {
        for (int x = 0; x < GASLINE_W; x++) {
            cell_t c = CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1);
            if (y >= GASLINE_H / 2 && (y + 7 * x) % (19 + (x % 3) * 6) == 0) {
                c = CELL_EMPTY;
            } else if (y == GASLINE_H / 4 && (x % 2) == 0) {
                c = CELL_EMPTY;
            } else if ((x % 5) == 3 && y == GASLINE_H / 2 + 5) {
                c = CELL_MAKE(MAT_STONE, 0);
            }
            sand_set(s, x, y, c);
        }
    }
}

/* Gas above and below a stone shelf with one gap, walled so nothing drifts
 * into it: the shelf holds no gas, so the sweep passes it by, and a column
 * run carried over it would hide the gap from the gas beside it. */
static void
scene_shelf_with_a_gap(sand_t* s) {
    const int gap_x = GASLINE_W / 2;
    const int shelf_y = GASLINE_H / 2;
    for (int y = 0; y < GASLINE_H; y++) {
        for (int x = 0; x < GASLINE_W; x++) {
            const bool shelf = y == shelf_y && x != gap_x;
            const bool wall = (x == gap_x - 1 || x == gap_x + 1) && (y == shelf_y - 1 || y == shelf_y + 1);
            cell_t c = CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1);
            if (shelf || wall) {
                c = CELL_MAKE(MAT_STONE, 0);
            } else if (y == shelf_y) {
                c = CELL_EMPTY;
            }
            sand_set(s, x, y, c);
        }
    }
}

/* Every cell gas, then one fire: the cascade a lit screen runs. */
static void
scene_packed_cascade(sand_t* s) {
    for (int y = 0; y < GASLINE_H; y++) {
        for (int x = 0; x < GASLINE_W; x++) {
            sand_set(s, x, y, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
        }
    }
    sand_set(s, GASLINE_W - 1, GASLINE_H / 2, CELL_MAKE(MAT_FIRE, 8));
}

/* The first step at which the two boards part, or -1. Frees both boards
 * before returning, so a failing assertion after it leaks nothing. */
static int
gas_line_parting_step(gas_line_scene_fn scene, uint32_t seed, int gx) {
    gas_lines_fixture();
    gas_line_board_open(&fast_board, scene, seed);
    gas_line_board_open(&slow_board, scene, seed);

    int parted = -1;
    for (int step = 0; step < GASLINE_STEPS && parted < 0; step++) {
        sand_gas_line_fast_paths_enable(true);
        sand_step(&fast_board.s, gx, 0, 0);
        sand_gas_line_fast_paths_enable(false);
        sand_step(&slow_board.s, gx, 0, 0);
        if (memcmp(fast_board.cells, slow_board.cells, (size_t)GASLINE_W * (size_t)GASLINE_H) != 0) {
            parted = step;
        }
    }
    gas_lines_teardown();
    return parted;
}

static void
expect_fast_paths_match_slow_path(gas_line_scene_fn scene, uint32_t seed) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, gas_line_parting_step(scene, seed, LANDSCAPE_GX),
                                  "gravity +x: the grids part at this step");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, gas_line_parting_step(scene, seed, -LANDSCAPE_GX),
                                  "gravity -x: the grids part at this step");
}

static void
test_pocketed_gas_spreads_the_same_with_the_fast_paths(void) {
    expect_fast_paths_match_slow_path(scene_pocketed_gas, 7U);
}

static void
test_burning_columns_spread_the_same_with_the_fast_paths(void) {
    expect_fast_paths_match_slow_path(scene_burning_columns, 19U);
}

static void
test_staggered_holes_spread_the_same_with_the_fast_paths(void) {
    expect_fast_paths_match_slow_path(scene_staggered_holes, 23U);
}

static void
test_a_gap_in_a_gas_free_shelf_spreads_the_same_with_the_fast_paths(void) {
    expect_fast_paths_match_slow_path(scene_shelf_with_a_gap, 29U);
}

static void
test_a_packed_cascade_spreads_the_same_with_the_fast_paths(void) {
    expect_fast_paths_match_slow_path(scene_packed_cascade, 17U);
}

void
run_sand_gas_lines_suite(void) {
    RUN_TEST(test_pocketed_gas_spreads_the_same_with_the_fast_paths);
    RUN_TEST(test_burning_columns_spread_the_same_with_the_fast_paths);
    RUN_TEST(test_staggered_holes_spread_the_same_with_the_fast_paths);
    RUN_TEST(test_a_gap_in_a_gas_free_shelf_spreads_the_same_with_the_fast_paths);
    RUN_TEST(test_a_packed_cascade_spreads_the_same_with_the_fast_paths);
}

SUITE_REGISTER(run_sand_gas_lines_suite);

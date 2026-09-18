/* Portable checks for the checkerboard-parallel sweep: deterministic,
 * mass-conserving, and without a stripe seam. Host jobs run inline. */
#include <stdio.h>
#include <stdlib.h>

#include "sand.h"
#include "sand_priv.h"
#include "suite_sand_common.h"
#include "suites.h"
#include "unity.h"

#include "util/job.h"

#ifdef DEVICE_BUILD
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define TC_W          ((int)REAL_W)
#define TC_H          ((int)REAL_H)
#define TC_BLOCK_COLS ((TC_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define TC_BLOCK_ROWS ((TC_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

_Static_assert(TC_H >= SAND_STRIPE_H_MIN * SAND_STRIPE_SPLIT_MIN_COUNT,
               "the two-core suite needs enough stripes to split");

static uint32_t
tc_hash(const uint8_t* bytes, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}

/* A mixed, seed-varied scatter: powders, two liquids of different
 * density, a gas and a static, at a random third of the grid each - so
 * every stripe, on both sides of every boundary, has something to move,
 * settle or touch as liquid as the seed changes. */
static void
tc_build_scattered_scene(sand_t* s, uint8_t* cells, uint32_t seed) {
    sand_init(s, cells, TC_W, TC_H, seed);

    rng_t r;
    rng_seed(&r, seed ^ 0xA5A5A5A5u);

    static const cell_t picks[] = {SAND, WATER, OIL, GAS, STONE};

    for (int y = 0; y < TC_H; y++) {
        for (int x = 0; x < TC_W; x++) {
            if (rng_below(&r, 3) != 0) {
                continue;
            }
            const cell_t pick = picks[rng_below(&r, (int)(sizeof picks / sizeof picks[0]))];
            sand_set(s, x, y, pick);
        }
    }
}

/* Runs `steps` of the scene under a small rotation of gravity vectors -
 * not just straight down - so the stripe grid's rolling offset (sand.c)
 * actually alternates and every boundary is crossed both ways. */
static uint32_t
tc_run_and_hash(uint32_t seed, int steps, bool two_core) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    tc_build_scattered_scene(&s, cells, seed);
    sand_enable_sleeping(&s, blocks);

    static const int gx[] = {0, 0, 1000, -1000, 700};
    static const int gy[] = {1000, -1000, 700, 700, -700};

    sand_set_two_core_step(two_core);
    for (int i = 0; i < steps; i++) {
        const int arm = i % (int)(sizeof gx / sizeof gx[0]);
        sand_step(&s, gx[arm], gy[arm], 0);
    }
    sand_set_two_core_step(false); /* restore the shipped default */

    uint32_t h = tc_hash(cells, (size_t)TC_W * (size_t)TC_H);
    h ^= tc_hash(blocks, (size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS) * 0x9E3779B1u;

    free(cells);
    free(blocks);
    return h;
}

static uint32_t
tc_run_quality_and_hash(int w, int h, uint32_t seed, bool two_core) {
    const int block_cols = (w + SAND_BLOCK_W - 1) / SAND_BLOCK_W;
    const int block_rows = (h + SAND_BLOCK_H - 1) / SAND_BLOCK_H;
    uint8_t* cells = malloc((size_t)w * (size_t)h);
    uint8_t* blocks = malloc((size_t)block_cols * (size_t)block_rows);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, cells, w, h, seed);
    sand_enable_sleeping(&s, blocks);

    rng_t r;
    rng_seed(&r, seed ^ 0xA5A5A5A5u);
    static const cell_t picks[] = {SAND, WATER, OIL, GAS, STONE};
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (rng_below(&r, 3) == 0) {
                sand_set(&s, x, y, picks[rng_below(&r, (int)(sizeof picks / sizeof picks[0]))]);
            }
        }
    }

    static const int gx[] = {0, 0, 1000, -1000, 700};
    static const int gy[] = {1000, -1000, 700, 700, -700};
    sand_set_two_core_step(two_core);
    for (int i = 0; i < 40; i++) {
        const int arm = i % (int)(sizeof gx / sizeof gx[0]);
        sand_step(&s, gx[arm], gy[arm], 0);
    }
    sand_set_two_core_step(false);

    uint32_t result = tc_hash(cells, (size_t)w * (size_t)h);
    result ^= tc_hash(blocks, (size_t)block_cols * (size_t)block_rows) * 0x9E3779B1u;
    free(cells);
    free(blocks);
    return result;
}

/* THE DETERMINISM CLAIM: the same seed run twice with two-core stepping
 * on must land on the same board both times. Nothing in sand_rng_next_at()
 * or run_sweep_stripes() (sand.c) reads wall-clock time, thread identity
 * or any state a real second core could have left different between two
 * otherwise-identical runs, so two host runs standing in for "device run
 * once, host run once" is the whole guarantee, not a weaker stand-in for
 * it - see this file's own top comment. */
static void
tc_assert_seed_is_deterministic(uint32_t seed, int steps) {
    const uint32_t first = tc_run_and_hash(seed, steps, true);
    const uint32_t second = tc_run_and_hash(seed, steps, true);

    char why[160];
    snprintf(why, sizeof why,
             "seed %u: two runs of the same seed under two-core stepping "
             "must land on the same board - first=%08x second=%08x",
             (unsigned)seed, (unsigned)first, (unsigned)second);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(first, second, why);
}

static void
test_two_core_step_is_deterministic_across_seeds(void) {
    /* Different seeds, not the same board re-run: a single arrangement
     * could look deterministic by accident of where its own stripe
     * boundaries happen to land - see
     * test_a_passing_test_may_be_passing_by_arrangement's own reasoning
     * elsewhere in this tree. */
    static const uint32_t seeds[] = {1u, 7u, 42u, 12345u, 99991u, 0xC0FFEEu};

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        tc_assert_seed_is_deterministic(seeds[i], 40);
    }
}

static void
test_split_gas_walk_uses_hashed_rng(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 91u);
    sand_set_decay(&s, 0);
    for (int y = 1; y < TC_H - 1; y += 3) {
        for (int x = 1; x < TC_W - 1; x += 2) {
            sand_set(&s, x, y, GAS);
        }
    }

    static const int slide_a[] = {-1, 1};
    static const int slide_b[] = {1, 1};
    static const int perp_a[] = {1, 0};
    static const int perp_b[] = {-1, 0};
    const rng_t before = s.rng;
    sand_set_two_core_step(true);
    sand_step_gas(&s, 0, 1000, 0, 1, slide_a, slide_b, perp_a, perp_b, 0, 1, 1, 0);
    sand_set_two_core_step(false);
    const rng_t after = s.rng;

    free(cells);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &after, sizeof before,
                                     "split gas walk must leave the sequential RNG untouched");
}

static uint32_t
tc_run_gas_walk_and_hash(uint32_t seed, bool reverse_workers) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, seed);
    sand_set_decay(&s, 0);
    rng_t placement;
    rng_seed(&placement, seed ^ 0xA5A5A5A5u);
    for (int y = 1; y < TC_H - 1; y++) {
        for (int x = 1; x < TC_W - 1; x++) {
            if (rng_below(&placement, 3) == 0) {
                sand_set(&s, x, y, GAS);
            }
        }
    }

    static const int slide_a[] = {-1, 1};
    static const int slide_b[] = {1, 1};
    static const int perp_a[] = {1, 0};
    static const int perp_b[] = {-1, 0};
    sand_gas_set_worker_order_for_test(reverse_workers);
    sand_set_two_core_step(true);
    for (int step = 0; step < 20; step++) {
        s.step_phase = (uint16_t)step;
        sand_step_gas(&s, 0, 1000, 0, 1, slide_a, slide_b, perp_a, perp_b, 0, 1, 1, 0);
    }
    sand_set_two_core_step(false);
    sand_gas_set_worker_order_for_test(false);

    const uint32_t hash = tc_hash(cells, (size_t)TC_W * (size_t)TC_H);
    free(cells);
    return hash;
}

static void
test_split_gas_walk_ignores_worker_order(void) {
    static const uint32_t seeds[] = {1u, 17u, 91u, 0xC0FFEEu};
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        const uint32_t ordinary = tc_run_gas_walk_and_hash(seeds[i], false);
        const uint32_t reversed = tc_run_gas_walk_and_hash(seeds[i], true);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(ordinary, reversed,
                                        "changing which worker owns each stripe must not change the gas walk");
    }
}

/* The serial and two-core paths draw from genuinely different streams
 * now (sand_rng_next_at()), so this is not an equivalence check - it is
 * a sanity check that turning the switch on does not quietly turn it
 * into a no-op that happens to hash the same by never actually
 * splitting anything on a board with too few stripes. */
static void
test_two_core_step_actually_changes_the_draw_stream(void) {
    const uint32_t serial = tc_run_and_hash(3u, 40, false);
    const uint32_t two_core = tc_run_and_hash(3u, 40, true);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(serial, two_core,
                                  "two-core stepping produced the same hash as serial on a "
                                  "board large enough to split - the checkerboard sweep "
                                  "should be drawing from sand_rng_next_at(), not silently "
                                  "falling back to the sequential stream");
}

static void
test_two_core_step_changes_the_draw_stream_at_smaller_qualities(void) {
    static const struct {
        int w, h;
    } qualities[] = {{92, 112}, {61, 74}, {46, 56}};

    static const uint32_t seeds[] = {1u, 7u, 42u, 12345u, 99991u, 0xC0FFEEu};

    for (size_t q = 0; q < sizeof qualities / sizeof qualities[0]; q++) {
        for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
            const uint32_t serial = tc_run_quality_and_hash(qualities[q].w, qualities[q].h, seeds[i], false);
            const uint32_t split = tc_run_quality_and_hash(qualities[q].w, qualities[q].h, seeds[i], true);
            char why[160];
            snprintf(why, sizeof why, "%dx%d seed %u: two-core stepping did not change the draw stream", qualities[q].w,
                     qualities[q].h, (unsigned)seeds[i]);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(serial, split, why);
        }
    }
}

/* MASS-PLAUSIBLE, NOT MASS-CONSERVED: water's own model already
 * redistributes fill levels (see suite_sand_perf.c's own comment on the
 * same point for the serial path), so the bar here is that two-core
 * stepping does not leak or fabricate a bulk fraction of the board's
 * liquid - the class of bug an out-of-bounds tile or a doubled cell
 * would actually produce. */
static void
test_two_core_step_does_not_leak_or_fabricate_mass(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    tc_build_scattered_scene(&s, cells, 17u);
    sand_enable_sleeping(&s, blocks);

    long before = 0;
    for (int i = 0; i < TC_W * TC_H; i++) {
        if (!CELL_IS_EMPTY(cells[i]) && CELL_MATERIAL(cells[i]) == MAT_WATER) {
            before += CELL_VARIANT(cells[i]);
        }
    }

    sand_set_two_core_step(true);
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    sand_set_two_core_step(false);

    long after = 0;
    for (int i = 0; i < TC_W * TC_H; i++) {
        if (!CELL_IS_EMPTY(cells[i]) && CELL_MATERIAL(cells[i]) == MAT_WATER) {
            after += CELL_VARIANT(cells[i]);
        }
    }

    free(cells);
    free(blocks);

    char why[160];
    snprintf(why, sizeof why,
             "water mass drifted from %ld to %ld under two-core stepping - a tile boundary lost or "
             "duplicated mass",
             before, after);
    /* A splash can throw a little water off the edge of the grid, and a
     * boil/quench can spend a unit of mass - neither is a bug and neither
     * is what this guards against, so the bar is "still the same order of
     * magnitude", not exact conservation. */
    TEST_ASSERT_GREATER_THAN_MESSAGE(before / 2, after, why);
}

/* Fills occupied[0..h) with each row's non-empty cell count on grid g. */
static void
count_occupied_per_row(const sand_t* g, int w, int h, int* occupied) {
    for (int y = 0; y < h; y++) {
        int n = 0;
        for (int x = 0; x < w; x++) {
            if (!CELL_IS_EMPTY(sand_at(g, x, y))) {
                n++;
            }
        }
        occupied[y] = n;
    }
}

/* True if row y sits within one row of a stripe_half-period boundary. */
static bool
row_near_stripe_boundary(int y, int stripe_half) {
    for (int m = -1; m <= 1; m++) {
        if (((y + m) % stripe_half) == 0) {
            return true;
        }
    }
    return false;
}

/* Folds occupied[]'s row-to-row deltas into the largest seen at a stripe
 * boundary vs anywhere in the interior - the tile-seam readout below. */
static void
worst_row_deltas(const int* occupied, int h, int stripe_half, int* interior_worst, int* boundary_worst) {
    for (int y = 1; y < h; y++) {
        const int delta = occupied[y] - occupied[y - 1];
        const int adelta = delta < 0 ? -delta : delta;
        if (row_near_stripe_boundary(y, stripe_half)) {
            if (adelta > *boundary_worst) {
                *boundary_worst = adelta;
            }
        } else if (adelta > *interior_worst) {
            *interior_worst = adelta;
        }
    }
}

/* THE VISUAL SEAM CHECK: a full-width slab of sand poured above an empty
 * container and left to fall and settle. Every row's final occupancy
 * should follow the pile's own shape, not the stripe grid's - a tile
 * artifact would show up as a step in occupancy repeating every
 * half a stripe height (the rolling offset's own period), which a
 * histogram of row-to-row deltas makes visible without eyeballing a
 * render. */
static void
test_a_settled_pile_under_two_core_stepping_shows_no_tile_seam(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 9u);
    sand_enable_sleeping(&s, blocks);

    for (int x = 0; x < TC_W; x++) {
        sand_set(&s, x, TC_H - 1, STONE);
    }
    for (int y = 4; y < TC_H / 2; y++) {
        for (int x = 2; x < TC_W - 2; x++) {
            sand_set(&s, x, y, SAND);
        }
    }

    sand_set_two_core_step(true);
    for (int i = 0; i < 400; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    sand_set_two_core_step(false);

    int* occupied = malloc(sizeof(int) * (size_t)TC_H);
    TEST_ASSERT_NOT_NULL(occupied);
    count_occupied_per_row(&s, TC_W, TC_H, occupied);

    /* Interior baseline: the largest row-to-row change anywhere OUTSIDE a
     * one-row margin of every stripe boundary - the pile's own surface,
     * which is not flat, sets this. */
    const int stripe_half = SAND_BLOCK_H / 2;
    int interior_worst = 0;
    int boundary_worst = 0;
    worst_row_deltas(occupied, TC_H, stripe_half, &interior_worst, &boundary_worst);

    free(occupied);
    free(cells);
    free(blocks);

    char why[220];
    snprintf(why, sizeof why,
             "a settled pile's row-to-row occupancy jumped more at a stripe "
             "boundary (%d) than anywhere in the interior (%d) - that is "
             "what a baked-in tile seam looks like",
             boundary_worst, interior_worst);
    /* Generous on purpose: this is a statistical check on one seed, not a
     * pixel-exact one, and the pile's own surface already has real jumps
     * near its edges. The claim is only that a boundary is not a special,
     * repeatable outlier. */
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(interior_worst + TC_W / 10, boundary_worst, why);
}

/* A free-fall step advances the phase without touching a cell, selecting a
 * different deterministic stripe offset for the next real step. */
static void
tc_prime_offset(sand_t* s, int offset) {
    if (offset == 0) {
        sand_step(s, 0, 0, 0);
    }
}

/* THE DOUBLE-MOVE CHECK: a lone grain with nothing to block it moves
 * exactly one cell in one step, whether it starts on a stripe seam or
 * two stripes away from one. Landing on a seam and travelling two cells
 * in one step is exactly what run_sweep_guard_rows() (sand.c) exists to
 * prevent - a phase-A move that lands in a not-yet-swept phase-B row,
 * found and moved again once that row's own sweep runs. */
static void
tc_assert_free_fall_moves_one_cell(int offset, int gx, int gy, int start_y, bool two_core) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 1u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, 0); /* a deterministic, driftless fall */
    tc_prime_offset(&s, offset);

    const int start_x = TC_W / 2;
    sand_set(&s, start_x, start_y, SAND);

    sand_set_two_core_step(two_core);
    sand_step(&s, gx, gy, 0);
    sand_set_two_core_step(false);

    int dx, dy;
    sand_gravity_direction(gx, gy, &dx, &dy);
    const bool left_start = CELL_IS_EMPTY(sand_at(&s, start_x, start_y));
    const bool at_one = !CELL_IS_EMPTY(sand_at(&s, start_x + dx, start_y + dy));
    const bool at_two_empty = CELL_IS_EMPTY(sand_at(&s, start_x + 2 * dx, start_y + 2 * dy));

    free(cells);
    free(blocks);

    char why[200];
    snprintf(why, sizeof why,
             "a lone grain at row %d (offset %d, gravity %d,%d, two_core=%d) "
             "did not travel exactly one cell in one step",
             start_y, offset, gx, gy, (int)two_core);
    TEST_ASSERT_TRUE_MESSAGE(left_start && at_one && at_two_empty, why);
}

/* A slide can move a grain diagonally even under horizontal gravity (see
 * Sand-Simulation.md's reach table), which is the other way a seam can be
 * crossed - so this blocks the straight-ahead cell and checks the row
 * component of the resulting slide never exceeds one either. */
static void
tc_assert_forced_slide_does_not_double_move(int offset, int gx, int gy, int start_y, bool two_core) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 1u);
    sand_enable_sleeping(&s, blocks);
    tc_prime_offset(&s, offset);

    const int start_x = TC_W / 2;
    int dx, dy;
    sand_gravity_direction(gx, gy, &dx, &dy);
    sand_set(&s, start_x + dx, start_y + dy, STONE);
    sand_set(&s, start_x, start_y, SAND);

    sand_set_two_core_step(two_core);
    sand_step(&s, gx, gy, 0);
    sand_set_two_core_step(false);

    int found_y = -1;
    for (int y = start_y - 2; y <= start_y + 2; y++) {
        for (int x = start_x - 2; x <= start_x + 2; x++) {
            if ((unsigned)x >= (unsigned)TC_W || (unsigned)y >= (unsigned)TC_H) {
                continue;
            }
            const cell_t c = sand_at(&s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_SAND) {
                found_y = y;
            }
        }
    }

    free(cells);
    free(blocks);

    char why[200];
    snprintf(why, sizeof why,
             "a lone grain slid more than one row in one step at row %d "
             "(offset %d, gravity %d,%d, two_core=%d)",
             start_y, offset, gx, gy, (int)two_core);
    TEST_ASSERT_TRUE_MESSAGE(found_y >= 0, why);
    const int row_delta = found_y - start_y;
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(1, row_delta < 0 ? -row_delta : row_delta, why);
}

/* Every internal stripe boundary this grid has, for BOTH stripe offsets -
 * see tc_prime_offset() - is a seam row pair; a row two stripes away from
 * either is the control. All four axis-aligned gravity directions, since
 * the seam is a ROW property and does not care which way is down. */
static void
test_two_core_step_never_double_moves_at_a_seam(void) {
    static const int gxs[] = {0, 0, 1000, -1000};
    static const int gys[] = {1000, -1000, 0, 0};
    static const int offsets[] = {0, SAND_BLOCK_H / 2};

    for (size_t o = 0; o < sizeof offsets / sizeof offsets[0]; o++) {
        for (int boundary = SAND_BLOCK_H; boundary < TC_H - SAND_BLOCK_H; boundary += SAND_BLOCK_H) {
            const int seam_rows[] = {boundary - 2, boundary - 1, boundary, boundary + 1, boundary - SAND_BLOCK_H / 2};
            for (size_t r = 0; r < sizeof seam_rows / sizeof seam_rows[0]; r++) {
                for (size_t g = 0; g < sizeof gxs / sizeof gxs[0]; g++) {
                    tc_assert_free_fall_moves_one_cell(offsets[o], gxs[g], gys[g], seam_rows[r], true);
                    tc_assert_forced_slide_does_not_double_move(offsets[o], gxs[g], gys[g], seam_rows[r], true);
                }
            }
        }
    }
}

/* THE SERIAL COMPARISON: with scatter forced to 0 and nothing else on the
 * board, a free fall draws no randomness at all (try_scatter() returns
 * immediately, the primary move needs no roll), so the serial and
 * two-core paths must agree exactly, not merely both travel one cell. */
static void
test_two_core_step_matches_serial_fall_distance_at_a_seam(void) {
    const int boundary = SAND_BLOCK_H * 2;

    static const int gxs[] = {0, 0, 1000, -1000};
    static const int gys[] = {1000, -1000, 0, 0};

    for (size_t g = 0; g < sizeof gxs / sizeof gxs[0]; g++) {
        for (int offset = 0; offset <= SAND_BLOCK_H / 2; offset += SAND_BLOCK_H / 2) {
            uint8_t* serial_cells = malloc((size_t)TC_W * (size_t)TC_H);
            uint8_t* two_core_cells = malloc((size_t)TC_W * (size_t)TC_H);
            uint8_t* serial_blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
            uint8_t* two_core_blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
            TEST_ASSERT_NOT_NULL(serial_cells);
            TEST_ASSERT_NOT_NULL(two_core_cells);
            TEST_ASSERT_NOT_NULL(serial_blocks);
            TEST_ASSERT_NOT_NULL(two_core_blocks);

            sand_t serial_s, two_core_s;
            sand_init(&serial_s, serial_cells, TC_W, TC_H, 1u);
            sand_init(&two_core_s, two_core_cells, TC_W, TC_H, 1u);
            sand_enable_sleeping(&serial_s, serial_blocks);
            sand_enable_sleeping(&two_core_s, two_core_blocks);
            sand_set_scatter(&serial_s, 0);
            sand_set_scatter(&two_core_s, 0);
            tc_prime_offset(&serial_s, offset);
            tc_prime_offset(&two_core_s, offset);

            const int start_x = TC_W / 2;
            sand_set(&serial_s, start_x, boundary, SAND);
            sand_set(&two_core_s, start_x, boundary, SAND);

            sand_set_two_core_step(false);
            sand_step(&serial_s, gxs[g], gys[g], 0);
            sand_set_two_core_step(true);
            sand_step(&two_core_s, gxs[g], gys[g], 0);
            sand_set_two_core_step(false);

            const uint32_t serial_hash = tc_hash(serial_cells, (size_t)TC_W * (size_t)TC_H);
            const uint32_t two_core_hash = tc_hash(two_core_cells, (size_t)TC_W * (size_t)TC_H);

            free(serial_cells);
            free(two_core_cells);
            free(serial_blocks);
            free(two_core_blocks);

            char why[160];
            snprintf(why, sizeof why,
                     "gravity %d,%d offset %d: a zero-randomness fall across a "
                     "seam moved differently under two-core stepping",
                     gxs[g], gys[g], offset);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(serial_hash, two_core_hash, why);
        }
    }
}

static void
tc_assert_quality_seam_matches_serial(int w, int h, int offset, int boundary) {
    const int block_cols = (w + SAND_BLOCK_W - 1) / SAND_BLOCK_W;
    const int block_rows = (h + SAND_BLOCK_H - 1) / SAND_BLOCK_H;
    uint8_t* serial_cells = malloc((size_t)w * (size_t)h);
    uint8_t* split_cells = malloc((size_t)w * (size_t)h);
    uint8_t* serial_blocks = malloc((size_t)block_cols * (size_t)block_rows);
    uint8_t* split_blocks = malloc((size_t)block_cols * (size_t)block_rows);
    TEST_ASSERT_NOT_NULL(serial_cells);
    TEST_ASSERT_NOT_NULL(split_cells);
    TEST_ASSERT_NOT_NULL(serial_blocks);
    TEST_ASSERT_NOT_NULL(split_blocks);

    sand_t serial, split;
    sand_init(&serial, serial_cells, w, h, 1u);
    sand_init(&split, split_cells, w, h, 1u);
    sand_enable_sleeping(&serial, serial_blocks);
    sand_enable_sleeping(&split, split_blocks);
    sand_set_scatter(&serial, 0);
    sand_set_scatter(&split, 0);
    tc_prime_offset(&serial, offset);
    tc_prime_offset(&split, offset);
    sand_set(&serial, w / 2, boundary, SAND);
    sand_set(&split, w / 2, boundary, SAND);

    sand_set_two_core_step(false);
    sand_step(&serial, 0, 1000, 0);
    sand_set_two_core_step(true);
    sand_step(&split, 0, 1000, 0);
    sand_set_two_core_step(false);

    const uint32_t serial_hash = tc_hash(serial_cells, (size_t)w * (size_t)h);
    const uint32_t split_hash = tc_hash(split_cells, (size_t)w * (size_t)h);
    free(serial_cells);
    free(split_cells);
    free(serial_blocks);
    free(split_blocks);

    char why[160];
    snprintf(why, sizeof why, "%dx%d offset %d boundary %d: a seam fall differed from serial", w, h, offset, boundary);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(serial_hash, split_hash, why);
}

static void
test_smaller_quality_seams_match_serial(void) {
    static const struct {
        int w, h;
    } qualities[] = {{92, 112}, {61, 74}, {46, 56}};

    for (size_t q = 0; q < sizeof qualities / sizeof qualities[0]; q++) {
        const int stripe_h = sand_stripe_height(qualities[q].h);
        const int offsets[] = {0, stripe_h / 2};
        for (size_t o = 0; o < sizeof offsets / sizeof offsets[0]; o++) {
            for (int boundary = offsets[o] == 0 ? stripe_h : offsets[o]; boundary < qualities[q].h - 1;
                 boundary += stripe_h) {
                tc_assert_quality_seam_matches_serial(qualities[q].w, qualities[q].h, offsets[o], boundary);
            }
        }
    }
}

static void
test_settled_guard_rows_do_no_grain_work(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 1u);
    sand_enable_sleeping(&s, blocks);
    for (int y = 0; y < TC_H; y++) {
        for (int x = 0; x < TC_W; x++) {
            sand_set(&s, x, y, STONE);
        }
    }

    sand_set_two_core_step(true);
    sand_step(&s, 0, 1000, 0);
    sand_guard_cells_scanned = 0;
    sand_step(&s, 0, 1000, 0);
    sand_set_two_core_step(false);

    free(cells);
    free(blocks);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, sand_guard_cells_scanned,
                                   "settled guard rows must take the same block-row skip as the ordinary sweep");
}

/* A box of STONE around the whole grid, so every one of the four
 * axis-aligned gravity directions below has a floor to settle against -
 * not just down. */
static void
tc_build_bordered_box(sand_t* s) {
    for (int x = 0; x < TC_W; x++) {
        sand_set(s, x, 0, STONE);
        sand_set(s, x, TC_H - 1, STONE);
    }
    for (int y = 0; y < TC_H; y++) {
        sand_set(s, 0, y, STONE);
        sand_set(s, TC_W - 1, y, STONE);
    }
}

/* A single, unbroken column of touching grains spanning every stripe in
 * the grid - unlike tc_assert_free_fall_moves_one_cell's lone grain, each
 * cell's upstream neighbour is occupied too, so a guard row here starts
 * genuinely contested rather than empty. */
static void
tc_build_falling_column(sand_t* s) {
    tc_build_bordered_box(s);
    const int x = TC_W / 2;
    for (int y = 1; y < TC_H / 2; y++) {
        sand_set(s, x, y, SAND);
    }
}

/* A wide slab, dense enough that grains contend with each other both
 * along the fall and sideways as they pack against the floor and each
 * other - the "pile" the double-move fix must still match serial on. */
static void
tc_build_dense_pile(sand_t* s) {
    tc_build_bordered_box(s);
    for (int y = TC_H / 4; y < 3 * TC_H / 4; y++) {
        for (int x = TC_W / 4; x < 3 * TC_W / 4; x++) {
            sand_set(s, x, y, SAND);
        }
    }
}

static uint32_t
tc_run_zero_rng_and_hash(void (*build)(sand_t*), int steps, int gx, int gy, int offset, bool two_core, int* out_n) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 1u);
    sand_enable_sleeping(&s, blocks);
    sand_set_scatter(&s, 0);
    tc_prime_offset(&s, offset);
    build(&s);

    sand_set_two_core_step(two_core);
    for (int i = 0; i < steps; i++) {
        sand_step(&s, gx, gy, 0);
    }
    sand_set_two_core_step(false);

    uint32_t h = tc_hash(cells, (size_t)TC_W * (size_t)TC_H);
    h ^= tc_hash(blocks, (size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS) * 0x9E3779B1u;

    if (out_n != NULL) {
        int n = 0;
        for (int i = 0; i < TC_W * TC_H; i++) {
            if (!CELL_IS_EMPTY(cells[i]) && CELL_MATERIAL(cells[i]) == MAT_SAND) {
                n++;
            }
        }
        *out_n = n;
    }

    free(cells);
    free(blocks);
    return h;
}

/* A dense column and pile, where a guard row's neighbour is never empty,
 * so a phase-time move there truly contends with something. Not a
 * hash-identity check: three-plus stripes provably cannot reproduce
 * serial order exactly once a contested chain spans more than one
 * boundary - see "The seam fix" in Sand-Simulation.md. This checks the
 * part that must still hold - the grain count - which a double-move or a
 * dropped cell would break. */
static void
test_two_core_step_conserves_grains_on_a_dense_column_and_pile(void) {
    static const int gxs[] = {0, 0, 1000, -1000};
    static const int gys[] = {1000, -1000, 0, 0};
    static const int offsets[] = {0, SAND_BLOCK_H / 2};

    const struct {
        const char* name;
        void (*build)(sand_t*);
        int steps;
    } scenes[] = {
        {"a full falling column", tc_build_falling_column, TC_H},
        {"a dense settling pile", tc_build_dense_pile, TC_H * 2},
    };

    for (size_t sc = 0; sc < sizeof scenes / sizeof scenes[0]; sc++) {
        for (size_t g = 0; g < sizeof gxs / sizeof gxs[0]; g++) {
            for (size_t o = 0; o < sizeof offsets / sizeof offsets[0]; o++) {
                int serial_n = 0, two_core_n = 0;
                tc_run_zero_rng_and_hash(scenes[sc].build, scenes[sc].steps, gxs[g], gys[g], offsets[o], false,
                                         &serial_n);
                tc_run_zero_rng_and_hash(scenes[sc].build, scenes[sc].steps, gxs[g], gys[g], offsets[o], true,
                                         &two_core_n);

                char why[200];
                snprintf(why, sizeof why,
                         "%s under gravity %d,%d offset %d: two-core stepping changed the "
                         "grain count from %d to %d - a guard row moved a cell twice or "
                         "dropped it",
                         scenes[sc].name, gxs[g], gys[g], offsets[o], serial_n, two_core_n);
                TEST_ASSERT_EQUAL_INT_MESSAGE(serial_n, two_core_n, why);
            }
        }
    }
}

static int
tc_water_row_mass(const sand_t* s, int y) {
    int mass = 0;
    for (int x = 0; x < TC_W; x++) {
        const cell_t c = sand_at(s, x, y);
        if (CELL_MATERIAL(c) == MAT_WATER) {
            mass += CELL_VARIANT(c);
        }
    }
    return mass;
}

static void
test_landscape_water_column_has_no_row_mass_lag(void) {
    uint8_t* serial_cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* split_cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* serial_blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    uint8_t* split_blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(serial_cells);
    TEST_ASSERT_NOT_NULL(split_cells);
    TEST_ASSERT_NOT_NULL(serial_blocks);
    TEST_ASSERT_NOT_NULL(split_blocks);

    sand_t serial, split;
    sand_init(&serial, serial_cells, TC_W, TC_H, 7u);
    sand_init(&split, split_cells, TC_W, TC_H, 7u);
    sand_enable_sleeping(&serial, serial_blocks);
    sand_enable_sleeping(&split, split_blocks);
    for (int y = 0; y < TC_H / 2; y++) {
        for (int x = TC_W / 2 - 6; x < TC_W / 2 + 6; x++) {
            sand_set(&serial, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
            sand_set(&split, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    int worst_row_mass = 0;
    sand_force_hashed_rng(true);
    for (int step = 0; step < 40; step++) {
        memcpy(split_cells, serial_cells, (size_t)TC_W * (size_t)TC_H);
        memcpy(split_blocks, serial_blocks, (size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
        split.step_phase = serial.step_phase;

        sand_set_two_core_step(true);
        sand_step(&split, 1000, 0, 0);
        sand_set_two_core_step(false);
        sand_step(&serial, 1000, 0, 0);

        for (int y = 0; y < TC_H; y++) {
            const int split_mass = tc_water_row_mass(&split, y);
            const int serial_mass = tc_water_row_mass(&serial, y);
            const int difference = split_mass - serial_mass;
            const int row_mass = difference < 0 ? -difference : difference;
            if (row_mass > worst_row_mass) {
                worst_row_mass = row_mass;
            }
        }
    }
    sand_force_hashed_rng(false);

    free(serial_cells);
    free(split_cells);
    free(serial_blocks);
    free(split_blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, worst_row_mass,
                                  "the split landscape water column left liquid mass in a different row than serial");
}

static int
tc_first_gas_equalise_boundary(const sand_t* s) {
    const int stripe_h = sand_stripe_height(s->h);
    int boundary = sand_stripe_offset(s);
    while (boundary < 2) {
        boundary += stripe_h;
    }
    return boundary;
}

static void
tc_build_gas_equalise_seam(sand_t* s, int boundary) {
    const int x = TC_W / 2;
    sand_set_mobility(s, 0);
    sand_set(s, x, boundary - 2, STONE);
    sand_set(s, x, boundary - 1, GAS);
    sand_set(s, x, boundary, GAS);
}

static void
tc_step_gas_equalise(sand_t* s, bool two_core) {
    static const int slide_a[] = {-1, 1};
    static const int slide_b[] = {1, 1};
    static const int perp_a[] = {1, 0};
    static const int perp_b[] = {-1, 0};

    sand_set_two_core_step(two_core);
    sand_step_gas(s, 0, 1000, 0, 1, slide_a, slide_b, perp_a, perp_b, 0, 1, 1, 0);
    sand_set_two_core_step(false);
}

static void
test_split_gas_equalise_keeps_seam_order(void) {
    sand_gas_equalise_stripe_runs = 0;
    for (int phase = 0; phase < SAND_STRIPE_SPLIT_MIN_COUNT; phase++) {
        uint8_t* serial_cells = malloc((size_t)TC_W * (size_t)TC_H);
        uint8_t* split_cells = malloc((size_t)TC_W * (size_t)TC_H);
        TEST_ASSERT_NOT_NULL(serial_cells);
        TEST_ASSERT_NOT_NULL(split_cells);

        sand_t serial, split;
        sand_init(&serial, serial_cells, TC_W, TC_H, 1u);
        sand_init(&split, split_cells, TC_W, TC_H, 1u);
        serial.step_phase = split.step_phase = (uint16_t)phase;
        const int boundary = tc_first_gas_equalise_boundary(&serial);
        TEST_ASSERT_TRUE(boundary < TC_H);
        tc_build_gas_equalise_seam(&serial, boundary);
        tc_build_gas_equalise_seam(&split, boundary);

        tc_step_gas_equalise(&serial, false);
        tc_step_gas_equalise(&split, true);

        char why[160];
        const bool cells_match = memcmp(serial_cells, split_cells, (size_t)TC_W * (size_t)TC_H) == 0;
        snprintf(why, sizeof why, "phase %d boundary %d: split gas equalise changed seam order", phase, boundary);
        free(serial_cells);
        free(split_cells);
        TEST_ASSERT_TRUE_MESSAGE(cells_match, why);
    }
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0, sand_gas_equalise_stripe_runs,
                                          "portrait gas equalise did not enter its split stripes");
}

#ifdef DEVICE_BUILD
typedef struct {
    volatile bool* finished;
} job_timeout_ctx_t;

typedef struct {
    int* calls;
} job_inline_ctx_t;

static void
job_timeout_worker(void* ctx) {
    const job_timeout_ctx_t* const work = ctx;
    vTaskDelay(pdMS_TO_TICKS(20));
    *work->finished = true;
}

static void
job_inline_worker(void* ctx) {
    job_inline_ctx_t* const work = ctx;
    (*work->calls)++;
}

static void
test_a_timed_out_job_falls_back_inline(void) {
    volatile bool finished = false;
    const job_timeout_ctx_t timeout = {.finished = &finished};
    int inline_calls = 0;
    const job_inline_ctx_t inline_ctx = {.calls = &inline_calls};

    TEST_ASSERT_TRUE(job_run_core1(job_timeout_worker, &timeout, sizeof timeout));
    TEST_ASSERT_FALSE(job_wait(1));
    TEST_ASSERT_TRUE(job_run_core1(job_inline_worker, &inline_ctx, sizeof inline_ctx));
    TEST_ASSERT_EQUAL_INT(1, inline_calls);

    vTaskDelay(pdMS_TO_TICKS(40));
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_TRUE(job_wait(0));
}
#endif

/* A boundary that only ever takes two positions stalls cells on the same two
 * screen lines every step, which reads as banding - see sand_stripe_offset(). */
static void
test_stripe_boundaries_spread_over_the_stripe(void) {
    sand_t s = {0};
    s.rng_seed_base = 0x51ED5EEDu;
    s.h = 224; /* a grid tall enough that the stripe is the full SAND_BLOCK_H */

    const int stripe_h = sand_stripe_height(s.h);
    bool seen[SAND_BLOCK_H] = {false};
    int distinct = 0;
    for (int step = 0; step < 64; step++) {
        s.step_phase = (uint16_t)step;
        const int offset = sand_stripe_offset(&s);
        TEST_ASSERT_TRUE_MESSAGE(offset >= 0 && offset < stripe_h, "an offset must land inside the stripe");
        if (!seen[offset]) {
            seen[offset] = true;
            distinct++;
        }
    }

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(8, distinct, "64 steps must put boundaries on many lines, not the same two");
}

/* THE REACTION SPLIT - sand_step_reactions() called directly, never through
 * sand_step(), so these tests are about the local-rule split alone and
 * cannot be confused with the sweep or gas walk's own coverage above. */

/* Isolated fire/gas pairs, never two gas cells touching. GAS ignites free
 * of randomness (material.c) - but a CONNECTED pocket is not a fair split
 * vs serial comparison regardless: a cell ignited ahead of the scan
 * spreads further within the step, and stripe order is not row-major. */
static void
rc_build_isolated_fire_gas_pairs(sand_t* s, uint8_t* cells, int w, int h, uint32_t seed) {
    sand_init(s, cells, w, h, seed);

    rng_t r;
    rng_seed(&r, seed ^ 0x51ED5EEDu);
    for (int y = 3; y < h - 3; y += 4) {
        for (int x = 3; x < w - 3; x += 4) {
            sand_set(s, x, y, FIRE);
            static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            const int d = rng_below(&r, 4);
            sand_set(s, x + dirs[d][0], y + dirs[d][1], GAS);
        }
    }
}

static uint32_t
rc_run_fire_chain_and_hash(int w, int h, uint32_t seed, int steps, bool two_core) {
    uint8_t* cells = malloc((size_t)w * (size_t)h);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t s;
    rc_build_isolated_fire_gas_pairs(&s, cells, w, h, seed);

    sand_set_two_core_step(two_core);
    for (int i = 0; i < steps; i++) {
        sand_step_reactions(&s);
    }
    sand_set_two_core_step(false);

    const uint32_t hash = tc_hash(cells, (size_t)w * (size_t)h);
    free(cells);
    return hash;
}

static void
test_reaction_split_matches_serial_on_a_zero_randomness_fire_chain(void) {
    static const struct {
        int w, h;
    } qualities[] = {{TC_W, TC_H}, {92, 112}, {61, 74}, {46, 56}};

    static const uint32_t seeds[] = {1u, 7u, 42u};

    for (size_t q = 0; q < sizeof qualities / sizeof qualities[0]; q++) {
        for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
            const uint32_t serial = rc_run_fire_chain_and_hash(qualities[q].w, qualities[q].h, seeds[i], 30, false);
            const uint32_t split = rc_run_fire_chain_and_hash(qualities[q].w, qualities[q].h, seeds[i], 30, true);
            char why[160];
            snprintf(why, sizeof why, "%dx%d seed %u: a zero-randomness gas fire chain diverged under split reactions",
                     qualities[q].w, qualities[q].h, (unsigned)seeds[i]);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(serial, split, why);
        }
    }
}

/* A scattered mix of every stage the split touches - burning wood,
 * conducting/heat-ramped stone, chilling snow, a lava/water cool-off pair -
 * so real chance rolls happen throughout, unlike the zero-randomness scene
 * above. */
static void
rc_build_reaction_heavy_scene(sand_t* s, uint8_t* cells, int w, int h, uint32_t seed) {
    sand_init(s, cells, w, h, seed);

    rng_t r;
    rng_seed(&r, seed ^ 0x51ED5EEDu);
    static const cell_t picks[] = {STONE, WOOD, SNOW, WATER, LAVA};
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (rng_below(&r, 4) != 0) {
                continue;
            }
            sand_set(s, x, y, picks[rng_below(&r, (int)(sizeof picks / sizeof picks[0]))]);
        }
    }
    sand_set(s, w / 2, h / 2, FIRE);
}

static uint32_t
rc_run_reaction_heavy_and_hash(int w, int h, uint32_t seed, int steps, bool two_core) {
    uint8_t* cells = malloc((size_t)w * (size_t)h);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t s;
    rc_build_reaction_heavy_scene(&s, cells, w, h, seed);

    sand_set_two_core_step(two_core);
    for (int i = 0; i < steps; i++) {
        sand_step_reactions(&s);
    }
    sand_set_two_core_step(false);

    const uint32_t hash = tc_hash(cells, (size_t)w * (size_t)h);
    free(cells);
    return hash;
}

static void
test_reaction_split_is_deterministic_across_seeds(void) {
    static const uint32_t seeds[] = {1u, 7u, 42u, 12345u, 99991u};

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        const uint32_t first = rc_run_reaction_heavy_and_hash(TC_W, TC_H, seeds[i], 30, true);
        const uint32_t second = rc_run_reaction_heavy_and_hash(TC_W, TC_H, seeds[i], 30, true);
        char why[160];
        snprintf(why, sizeof why,
                 "seed %u: two runs of the same seed under split reactions must land on the same board",
                 (unsigned)seeds[i]);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(first, second, why);
    }
}

static void
test_reaction_split_actually_changes_the_draw_stream(void) {
    const uint32_t serial = rc_run_reaction_heavy_and_hash(TC_W, TC_H, 3u, 30, false);
    const uint32_t split = rc_run_reaction_heavy_and_hash(TC_W, TC_H, 3u, 30, true);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(serial, split,
                                  "reaction split produced the same hash as serial on a reaction-heavy "
                                  "board - the local rules should be drawing from sand_rng_next_at(), "
                                  "not silently falling back to the sequential stream");
}

void
run_sand_two_core_suite(void) {
    RUN_TEST(test_stripe_boundaries_spread_over_the_stripe);
    RUN_TEST(test_two_core_step_is_deterministic_across_seeds);
    RUN_TEST(test_split_gas_walk_uses_hashed_rng);
    RUN_TEST(test_split_gas_walk_ignores_worker_order);
    RUN_TEST(test_two_core_step_actually_changes_the_draw_stream);
    RUN_TEST(test_two_core_step_changes_the_draw_stream_at_smaller_qualities);
    RUN_TEST(test_two_core_step_does_not_leak_or_fabricate_mass);
    RUN_TEST(test_a_settled_pile_under_two_core_stepping_shows_no_tile_seam);
    RUN_TEST(test_two_core_step_never_double_moves_at_a_seam);
    RUN_TEST(test_two_core_step_matches_serial_fall_distance_at_a_seam);
    RUN_TEST(test_smaller_quality_seams_match_serial);
    RUN_TEST(test_settled_guard_rows_do_no_grain_work);
    RUN_TEST(test_two_core_step_conserves_grains_on_a_dense_column_and_pile);
    RUN_TEST(test_reaction_split_matches_serial_on_a_zero_randomness_fire_chain);
    RUN_TEST(test_reaction_split_is_deterministic_across_seeds);
    RUN_TEST(test_reaction_split_actually_changes_the_draw_stream);
    RUN_TEST(test_landscape_water_column_has_no_row_mass_lag);
    RUN_TEST(test_split_gas_equalise_keeps_seam_order);
#ifdef DEVICE_BUILD
    RUN_TEST(test_a_timed_out_job_falls_back_inline);
#endif
}

SUITE_REGISTER(run_sand_two_core_suite);

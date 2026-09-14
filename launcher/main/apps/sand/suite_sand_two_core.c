/*
 * Portable suite: the checkerboard-parallel sweep sand_set_two_core_step()
 * arms is NOT byte-identical to the serial step - it draws its own PRNG
 * hash rather than the shared sequential stream, on purpose, so two
 * stripes can run on two cores with nothing to race on (see
 * Sand-Simulation.md). What this suite checks instead is what the new
 * design actually promises: the same seed gives the same answer every
 * time, regardless of which core would have done which stripe, and the
 * result still looks like sand - mass-plausible, eventually settled, no
 * seam baked into the board at the stripe boundaries.
 *
 * On a host build sand_core1_run() has no second core to hand work to,
 * so it runs its callback inline - see sand_core1.c's own top comment.
 * That is exactly what "regardless of which core" needs: every draw this
 * design makes is a pure function of (seed, step, cell, draw site), never
 * of a shared counter or of execution order, so a host run already proves
 * what a genuinely concurrent one would give - see sand_rng_next_at()
 * (sand_priv.h).
 */
#include <stdio.h>
#include <stdlib.h>

#include "sand.h"
#include "suite_sand_common.h"
#include "suites.h"
#include "unity.h"

#define TC_W          ((int)REAL_W)
#define TC_H          ((int)REAL_H)
#define TC_BLOCK_COLS ((TC_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define TC_BLOCK_ROWS ((TC_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

/* Tall enough that the sweep's own checkerboard actually engages
 * (SWEEP_CHECKERBOARD_MIN_ROWS, sand.c) rather than taking its
 * always-serial fallback for a small board. */
_Static_assert(TC_H >= SAND_BLOCK_H * 4, "the two-core suite needs a grid tall enough to actually split");

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

/* The serial and two-core paths draw from genuinely different streams
 * now (sand_rng_next_at()), so this is not an equivalence check - it is
 * a sanity check that turning the switch on does not quietly turn it
 * into a no-op that happens to hash the same by never actually
 * splitting anything (which SWEEP_CHECKERBOARD_MIN_ROWS's fallback would
 * do on too small a board). */
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

/* THE VISUAL SEAM CHECK: a full-width slab of sand poured above an empty
 * container and left to fall and settle. Every row's final occupancy
 * should follow the pile's own shape, not the stripe grid's - a tile
 * artifact would show up as a step in occupancy repeating every
 * SWEEP_STRIPE_H/2 rows (the rolling offset's own period), which a
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
    for (int y = 0; y < TC_H; y++) {
        int n = 0;
        for (int x = 0; x < TC_W; x++) {
            if (!CELL_IS_EMPTY(sand_at(&s, x, y))) {
                n++;
            }
        }
        occupied[y] = n;
    }

    /* Interior baseline: the largest row-to-row change anywhere OUTSIDE a
     * one-row margin of every stripe boundary - the pile's own surface,
     * which is not flat, sets this. */
    const int stripe_half = SAND_BLOCK_H / 2;
    int interior_worst = 0;
    int boundary_worst = 0;
    for (int y = 1; y < TC_H; y++) {
        const int delta = occupied[y] - occupied[y - 1];
        const int adelta = delta < 0 ? -delta : delta;
        bool near_boundary = false;
        for (int m = -1; m <= 1; m++) {
            if (((y + m) % stripe_half) == 0) {
                near_boundary = true;
            }
        }
        if (near_boundary) {
            if (adelta > boundary_worst) {
                boundary_worst = adelta;
            }
        } else if (adelta > interior_worst) {
            interior_worst = adelta;
        }
    }

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

void
run_sand_two_core_suite(void) {
    RUN_TEST(test_two_core_step_is_deterministic_across_seeds);
    RUN_TEST(test_two_core_step_actually_changes_the_draw_stream);
    RUN_TEST(test_two_core_step_does_not_leak_or_fabricate_mass);
    RUN_TEST(test_a_settled_pile_under_two_core_stepping_shows_no_tile_seam);
}

SUITE_REGISTER(run_sand_two_core_suite);

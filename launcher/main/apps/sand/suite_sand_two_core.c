/*
 * Portable suite: sand_set_two_core_step() must never change what a step
 * computes, only which core computes part of it.
 *
 * On a host build sand_core1_run() has no second core to hand work to, so
 * it runs its callback inline instead - see sand_core1.c's own top
 * comment. That is exactly what this suite needs: it exercises the same
 * range-split code finalize_settling() (sand.c) and
 * mark_liquid_neighbourhoods() (sand_liquid.c) take on device, and checks
 * it against the plain, unsplit call, across scenes varied by seed so a
 * pass cannot be an accident of one arrangement - see
 * test_a_passing_test_may_be_passing_by_arrangement's own reasoning
 * elsewhere in this tree.
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

/* Enough block rows to exercise the split (FINALIZE_SETTLING_SPLIT_MIN_
 * BLOCK_ROWS and its liquid counterpart are both 4, sand.c/sand_liquid.c),
 * not merely its always-serial fallback for a small board. */
_Static_assert(TC_BLOCK_ROWS >= 4, "the two-core suite needs a grid tall enough to actually split");

static uint32_t
tc_hash(const uint8_t* bytes, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}

/* A mixed, seed-varied scatter: powders, two liquids of different density,
 * a gas and a static, at a random third of the grid each - dense enough
 * that blocks on both sides of the mid-height split are awake, settling
 * and touching liquid at different times as the seed changes. */
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
 * not just straight down - so blocks repeatedly cross the settled/active
 * boundary the two block-bookkeeping scans exist to track. */
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
    sand_set_two_core_step(true); /* restore the shipped default */

    uint32_t h = tc_hash(cells, (size_t)TC_W * (size_t)TC_H);
    h ^= tc_hash(blocks, (size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS) * 0x9E3779B1u;

    free(cells);
    free(blocks);
    return h;
}

static void
tc_assert_seed_matches(uint32_t seed, int steps) {
    const uint32_t serial = tc_run_and_hash(seed, steps, false);
    const uint32_t two_core = tc_run_and_hash(seed, steps, true);

    char why[160];
    snprintf(why, sizeof why,
             "seed %u: two-core stepping changed the grid or block state - "
             "serial=%08x two_core=%08x",
             (unsigned)seed, (unsigned)serial, (unsigned)two_core);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(serial, two_core, why);
}

static void
test_two_core_step_matches_serial_across_seeds(void) {
    /* Different seeds, not the same board re-hashed: a single arrangement
     * could pass because its own split point happens to land somewhere
     * uneventful, not because the split is actually order-independent. */
    static const uint32_t seeds[] = {1u, 7u, 42u, 12345u, 99991u, 0xC0FFEEu};

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        tc_assert_seed_matches(seeds[i], 40);
    }
}

/* A board with no block_state at all must take finalize_settling()'s and
 * mark_liquid_neighbourhoods()'s early return regardless of the switch -
 * this is what proves the split is opt-in machinery layered over the
 * existing sleeping-disabled path, not a second code path with its own
 * behaviour. */
static void
test_two_core_step_is_a_no_op_without_block_sleeping(void) {
    uint8_t* cells_a = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* cells_b = malloc((size_t)TC_W * (size_t)TC_H);
    TEST_ASSERT_NOT_NULL(cells_a);
    TEST_ASSERT_NOT_NULL(cells_b);

    sand_t a, b;
    tc_build_scattered_scene(&a, cells_a, 5u);
    tc_build_scattered_scene(&b, cells_b, 5u);
    /* Neither calls sand_enable_sleeping(): s->block_state stays NULL. */

    sand_set_two_core_step(false);
    for (int i = 0; i < 20; i++) {
        sand_step(&a, 0, 1000, 0);
    }
    sand_set_two_core_step(true);
    for (int i = 0; i < 20; i++) {
        sand_step(&b, 0, 1000, 0);
    }
    sand_set_two_core_step(true);

    const uint32_t hash_a = tc_hash(cells_a, (size_t)TC_W * (size_t)TC_H);
    const uint32_t hash_b = tc_hash(cells_b, (size_t)TC_W * (size_t)TC_H);

    free(cells_a);
    free(cells_b);

    TEST_ASSERT_EQUAL_HEX32_MESSAGE(hash_a, hash_b,
                                    "a board with sleeping disabled must step identically regardless of "
                                    "sand_set_two_core_step() - there is no block_state for either scan "
                                    "to split");
}

void
run_sand_two_core_suite(void) {
    RUN_TEST(test_two_core_step_matches_serial_across_seeds);
    RUN_TEST(test_two_core_step_is_a_no_op_without_block_sleeping);
}

SUITE_REGISTER(run_sand_two_core_suite);

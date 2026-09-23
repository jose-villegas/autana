/* Portable checks for the chunk-parallel passes - the gravity sweep on its
 * schedule, cross-flow, gas and reactions all on it: deterministic,
 * mass-conserving, and without a chunk seam. Host jobs run inline. */
#include <stdio.h>
#include <stdlib.h>

#include "apps/sand/sand.h"
#include "apps/sand/sand_priv.h"
#include "apps/sand/tests/suite_sand_common.h"
#include "suites.h"
#include "unity.h"

#include "util/job.h"

#ifdef HOST_HEAP_ARENA
#include "heap_arena.h"
#endif

#ifdef DEVICE_BUILD
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define TC_W          ((int)REAL_W)
#define TC_H          ((int)REAL_H)
#define TC_BLOCK_COLS ((TC_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define TC_BLOCK_ROWS ((TC_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

_Static_assert(TC_H >= 2 * SAND_CHUNK_SIDE_MIN && TC_W >= 2 * SAND_CHUNK_SIDE_MIN,
               "the two-core suite needs a board sand_chunk_plan() can cut both ways");

static const int tc_ring[][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};

/* The seam scenes place cells by one side on BOTH axes and sweep every
 * gravity, so the borders they aim at must not move with the travel class:
 * they force a square cut, and the force is what also carries the smaller
 * grids past SAND_CHUNK_SPLIT_MIN_CELLS. A square side near a twentieth of
 * the board keeps the chunk count, and so the scene count, where it was.
 * Paired with tc_release_square_side(). */
static int
tc_force_square_side(int w, int h) {
    const int target = (w * h) / 20;
    int side = 1;

    while (side <= target / side) {
        side++;
    }
    side--;
    if (side < SAND_CHUNK_SIDE_MIN) {
        side = SAND_CHUNK_SIDE_MIN;
    }
    TEST_ASSERT_TRUE_MESSAGE(sand_chunk_side_for_test(side, side), "the seam scenes' square cut must clear the floor");
    return side;
}

static void
tc_release_square_side(void) {
    (void)sand_chunk_side_for_test(0, 0);
}

/* A lane whose join timed out is still inside the board, so a scene that ends
 * on one hands the next scene a core still writing into memory this one is
 * about to free and malloc() is about to hand back. Every scene below
 * collects it before reading what it wrote. */
static void
tc_collect_core1(void) {
    for (int tries = 0; tries < 20 && !job_wait(100); tries++) {}
    TEST_ASSERT_TRUE_MESSAGE(job_wait(0), "a core-1 lane never came back");
}

static uint32_t
tc_hash(const uint8_t* bytes, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}

static void
tc_scatter(sand_t* s, uint8_t* cells, uint32_t seed, const cell_t* picks, int count) {
    sand_init(s, cells, TC_W, TC_H, seed);

    rng_t r;
    rng_seed(&r, seed ^ 0xA5A5A5A5u);

    for (int y = 0; y < TC_H; y++) {
        for (int x = 0; x < TC_W; x++) {
            if (rng_below(&r, 3) != 0) {
                continue;
            }
            sand_set(s, x, y, picks[rng_below(&r, count)]);
        }
    }
}

/* A mixed, seed-varied scatter: powders, two liquids of different
 * density, a gas and a static, at a random third of the grid each - so
 * every chunk, on both sides of every boundary, has something to move,
 * settle or touch as liquid as the seed changes. */
static void
tc_build_scattered_scene(sand_t* s, uint8_t* cells, uint32_t seed) {
    static const cell_t picks[] = {SAND, WATER, OIL, GAS, STONE};

    tc_scatter(s, cells, seed, picks, (int)(sizeof picks / sizeof picks[0]));
}

/* The same mix with both liquids left out, so a board just as busy carries
 * none of the liquid bookkeeping. */
static void
tc_build_dry_scattered_scene(sand_t* s, uint8_t* cells, uint32_t seed) {
    static const cell_t picks[] = {SAND, SAND, GAS, STONE};

    tc_scatter(s, cells, seed, picks, (int)(sizeof picks / sizeof picks[0]));
}

/* Mostly liquid, at every mass a cell can hold: cross-flow has somewhere to
 * move mass on every ray, which a board of full cells never gives it. */
static void
tc_build_liquid_scene(sand_t* s, uint8_t* cells, uint32_t seed) {
    sand_init(s, cells, TC_W, TC_H, seed);

    rng_t r;
    rng_seed(&r, seed ^ 0x5A5A5A5Au);

    for (int y = 0; y < TC_H; y++) {
        for (int x = 0; x < TC_W; x++) {
            const int pick = rng_below(&r, 8);
            if (pick == 0) {
                sand_set(s, x, y, STONE);
            } else if (pick < 6) {
                const material_id_t id = (pick < 5) ? MAT_WATER : MAT_OIL;
                sand_set(s, x, y, CELL_MAKE(id, (uint8_t)(1 + rng_below(&r, MASS_MAX))));
            }
        }
    }
}

/* An arm that named itself split and then swept on one core would agree with
 * the serial arm for the wrong reason. Such an arm asks for the split by
 * name; this is what says it was given it. */
static void
tc_assert_split_arm_swept_split(unsigned before, bool two_core) {
    if (two_core) {
        TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(before, sand_split_dispatches[SAND_SPLIT_SLOT_SWEEP],
                                              "a split arm swept every one of its steps on one core");
    }
}

/* Runs `steps` of the scene under a small rotation of gravity vectors -
 * not just straight down - so a chunk boundary is crossed both ways and in
 * both axes. */
static uint32_t
tc_run_scene_and_hash(void (*build)(sand_t*, uint8_t*, uint32_t), uint32_t seed, int steps, bool two_core) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    build(&s, cells, seed);
    sand_enable_sleeping(&s, blocks);
    void* scratch = lane_scratch_open(&s);

    static const int gx[] = {0, 0, 1000, -1000, 700};
    static const int gy[] = {1000, -1000, 700, 700, -700};

    const unsigned swept_before = sand_split_dispatches[SAND_SPLIT_SLOT_SWEEP];
    const sand_chunk_share_t share =
        sand_chunk_share_for_test(two_core ? SAND_CHUNK_SHARE_ALWAYS : SAND_CHUNK_SHARE_AUTO);
    sand_set_two_core_step(two_core);
    for (int i = 0; i < steps; i++) {
        const int arm = i % (int)(sizeof gx / sizeof gx[0]);
        sand_step(&s, gx[arm], gy[arm], 0);
    }
    sand_set_two_core_step(false);
    (void)sand_chunk_share_for_test(share);
    tc_collect_core1(); /* restore the shipped default */
    tc_assert_split_arm_swept_split(swept_before, two_core);

    uint32_t h = tc_hash(cells, (size_t)TC_W * (size_t)TC_H);
    h ^= tc_hash(blocks, (size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS) * 0x9E3779B1u;

    free(scratch);
    free(cells);
    free(blocks);
    return h;
}

static uint32_t
tc_run_and_hash(uint32_t seed, int steps, bool two_core) {
    return tc_run_scene_and_hash(tc_build_scattered_scene, seed, steps, two_core);
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
    void* scratch = lane_scratch_open(&s);

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
    /* Same reason as tc_run_scene_and_hash(); not checked, because half the
     * grids below sit under the floor on purpose and are meant to decline. */
    const sand_chunk_share_t share =
        sand_chunk_share_for_test(two_core ? SAND_CHUNK_SHARE_ALWAYS : SAND_CHUNK_SHARE_AUTO);
    sand_set_two_core_step(two_core);
    for (int i = 0; i < 40; i++) {
        const int arm = i % (int)(sizeof gx / sizeof gx[0]);
        sand_step(&s, gx[arm], gy[arm], 0);
    }
    sand_set_two_core_step(false);
    (void)sand_chunk_share_for_test(share);
    tc_collect_core1();

    uint32_t result = tc_hash(cells, (size_t)w * (size_t)h);
    result ^= tc_hash(blocks, (size_t)block_cols * (size_t)block_rows) * 0x9E3779B1u;
    free(scratch);
    free(cells);
    free(blocks);
    return result;
}

/* THE DETERMINISM CLAIM: the same seed run twice with two-core stepping
 * on must land on the same board both times. Nothing in sand_rng_next_at()
 * or the chunk passes (sand.c) read wall-clock time, thread identity
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
     * could look deterministic by accident of where its own chunk
     * boundaries happen to land - see
     * test_a_passing_test_may_be_passing_by_arrangement's own reasoning
     * elsewhere in this tree. */
    static const uint32_t seeds[] = {1u, 7u, 42u, 12345u, 99991u, 0xC0FFEEu};

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        tc_assert_seed_is_deterministic(seeds[i], 40);
    }
}

#define TC_DRIVERS     3

/* Step phases a seam check walks. The order alternates a line's chunks by
 * parity, so two cover it; four keeps a longer period honest as well. */
#define TC_SEAM_PHASES 4

/* A chunk's lane is its position's parity whoever executes it, and lane-keyed
 * state is what a chunk's sweep writes into, so the board a schedule produces
 * cannot depend on how the two lanes take turns. Driven on one thread here,
 * which is the only way to vary the turns deliberately. */
static void
test_the_split_sweep_ignores_how_its_lanes_interleave(void) {
    static const sand_chunk_pass_driver_t drivers[TC_DRIVERS] = {
        SAND_CHUNK_PASS_LANE0_EAGER, SAND_CHUNK_PASS_LANE1_EAGER, SAND_CHUNK_PASS_ALTERNATE};
    static const uint32_t seeds[] = {1u, 7u, 12345u};

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_SOLO);
        const uint32_t solo = tc_run_and_hash(seeds[i], 40, true);
        uint32_t driven[TC_DRIVERS];

        for (int d = 0; d < TC_DRIVERS; d++) {
            sand_chunk_pass_set_driver_for_test(drivers[d]);
            driven[d] = tc_run_and_hash(seeds[i], 40, true);
        }
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);

        for (int d = 0; d < TC_DRIVERS; d++) {
            char why[160];
            snprintf(why, sizeof why, "seed %u driver %d: the split sweep's board depended on the lane interleaving",
                     (unsigned)seeds[i], (int)drivers[d]);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(solo, driven[d], why);
        }
    }
}

/* Cross-flow rides the same schedule, so a mostly-liquid board owes the same
 * answer. Rotating gravity turns the pass's own travel direction with it. */
static void
test_the_split_liquid_pass_ignores_how_its_lanes_interleave(void) {
    static const sand_chunk_pass_driver_t drivers[TC_DRIVERS] = {
        SAND_CHUNK_PASS_LANE0_EAGER, SAND_CHUNK_PASS_LANE1_EAGER, SAND_CHUNK_PASS_ALTERNATE};
    static const uint32_t seeds[] = {3u, 19u, 65521u};
    const split_passes_scope_t crossflow = split_passes_scope_begin(SAND_SPLIT_CROSSFLOW);

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_SOLO);
        const uint32_t solo = tc_run_scene_and_hash(tc_build_liquid_scene, seeds[i], 40, true);
        uint32_t driven[TC_DRIVERS];

        for (int d = 0; d < TC_DRIVERS; d++) {
            sand_chunk_pass_set_driver_for_test(drivers[d]);
            driven[d] = tc_run_scene_and_hash(tc_build_liquid_scene, seeds[i], 40, true);
        }
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);

        for (int d = 0; d < TC_DRIVERS; d++) {
            char why[160];
            snprintf(why, sizeof why, "seed %u driver %d: a split liquid board depended on the lane interleaving",
                     (unsigned)seeds[i], (int)drivers[d]);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(solo, driven[d], why);
        }
    }
    split_passes_scope_end(crossflow);
}

#ifdef DEVICE_BUILD
static void
tc_no_op_job(void* ctx) {
    (void)ctx;
}

/* The claim the schedule exists for, made where a second core really takes
 * lane 1: the board two cores leave must be the one a single thread walking
 * the order leaves. Every interleaving a host can stage is a guess at this;
 * only a real core answers it. The refused dispatch is asserted because a
 * worker that failed to come up would run both halves solo and agree with
 * itself. */
static void
test_a_core_1_lane_lands_on_the_solo_board(void) {
    static const uint32_t seeds[] = {1u, 7u, 12345u};
    const split_passes_scope_t crossflow = split_passes_scope_begin(SAND_SPLIT_CROSSFLOW);

    TEST_ASSERT_TRUE_MESSAGE(job_try_core1(tc_no_op_job, NULL, 0), "no core-1 worker to compare against");
    TEST_ASSERT_TRUE(job_wait(100));

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        const uint32_t duo_sweep = tc_run_and_hash(seeds[i], 40, true);
        const uint32_t duo_liquid = tc_run_scene_and_hash(tc_build_liquid_scene, seeds[i], 40, true);

        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_SOLO);
        const uint32_t solo_sweep = tc_run_and_hash(seeds[i], 40, true);
        const uint32_t solo_liquid = tc_run_scene_and_hash(tc_build_liquid_scene, seeds[i], 40, true);
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);

        char why[160];
        snprintf(why, sizeof why, "seed %u: two cores and the single-thread walk parted on the sweep",
                 (unsigned)seeds[i]);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(solo_sweep, duo_sweep, why);
        snprintf(why, sizeof why, "seed %u: two cores and the single-thread walk parted on the liquids",
                 (unsigned)seeds[i]);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(solo_liquid, duo_liquid, why);
    }
    split_passes_scope_end(crossflow);
}
#endif

static void
test_split_gas_walk_uses_hashed_rng(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 91u);
    void* scratch = lane_scratch_open(&s);
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

    free(scratch);
    free(cells);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &after, sizeof before,
                                     "split gas walk must leave the sequential RNG untouched");
}

static uint32_t
tc_run_gas_and_hash(uint32_t seed, int gx, int gy, int dx, int dy) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, seed);
    void* scratch = lane_scratch_open(&s);
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

    /* The same ring picks sand_step() makes for this gravity. */
    const int ring = ring_of(dx, dy);
    const int* const slide_a = ring_dir(ring + 7);
    const int* const slide_b = ring_dir(ring + 1);
    const int* const perp_a = ring_dir(ring + 2);
    const int* const perp_b = ring_dir(ring + 6);
    sand_set_two_core_step(true);
    for (int step = 0; step < 20; step++) {
        s.step_phase = (uint16_t)step;
        sand_step_gas(&s, gx, gy, dx, dy, slide_a, slide_b, perp_a, perp_b, 0, 1, 1, 0);
    }
    sand_set_two_core_step(false);

    const uint32_t hash = tc_hash(cells, (size_t)TC_W * (size_t)TC_H);
    free(scratch);
    free(cells);
    return hash;
}

/* Both gas passes ride the same schedule, so a gas-heavy board owes the same
 * answer however the lanes take turns - and under every gravity, since each
 * turns both passes' travel with it. */
static void
test_the_split_gas_passes_ignore_how_their_lanes_interleave(void) {
    static const sand_chunk_pass_driver_t drivers[TC_DRIVERS] = {
        SAND_CHUNK_PASS_LANE0_EAGER, SAND_CHUNK_PASS_LANE1_EAGER, SAND_CHUNK_PASS_ALTERNATE};
    static const uint32_t seeds[] = {1u, 17u, 91u};

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        const size_t g = i % (sizeof tc_ring / sizeof tc_ring[0]);
        const int dx = tc_ring[g][0];
        const int dy = tc_ring[g][1];

        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_SOLO);
        const uint32_t solo = tc_run_gas_and_hash(seeds[i], dx * 1000, dy * 1000, dx, dy);
        uint32_t driven[TC_DRIVERS];

        for (int d = 0; d < TC_DRIVERS; d++) {
            sand_chunk_pass_set_driver_for_test(drivers[d]);
            driven[d] = tc_run_gas_and_hash(seeds[i], dx * 1000, dy * 1000, dx, dy);
        }
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);

        for (int d = 0; d < TC_DRIVERS; d++) {
            char why[160];
            snprintf(why, sizeof why, "seed %u gravity %d,%d driver %d: a split gas board depended on the interleaving",
                     (unsigned)seeds[i], dx, dy, (int)drivers[d]);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(solo, driven[d], why);
        }
    }
}

/* WHAT LETS THE SPREAD HOP PAST A WHOLE CHUNK WITH NO ARRIVAL MARK: the hop
 * runs along the ray, the chunk order runs against it, so the chunk a hop
 * lands in is one already finished - the one two along as well. Nothing else
 * keeps a grain to one hop per pass, which is why this counts rather than
 * samples, and why it also runs with the order reversed. */
static void
test_split_gas_spread_never_hops_into_a_chunk_ranked_later(void) {
    static const uint32_t seeds[] = {1u, 17u, 91u};
    const unsigned runs_before = sand_gas_equalise_runs;
    unsigned count[2];

    for (int reversed = 0; reversed < 2; reversed++) {
        sand_gas_late_arrivals = 0;
        sand_gas_rank_audit_enable(true, reversed != 0);
        for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
            for (size_t g = 0; g < sizeof tc_ring / sizeof tc_ring[0]; g++) {
                const int dx = tc_ring[g][0];
                const int dy = tc_ring[g][1];
                (void)tc_run_gas_and_hash(seeds[i], dx * 1000, dy * 1000, dx, dy);
            }
        }
        sand_gas_rank_audit_enable(false, false);
        count[reversed] = sand_gas_late_arrivals;
    }

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(runs_before, sand_gas_equalise_runs,
                                          "the scenes must enter the split spread for the count to mean anything");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, count[0], "gas spread hopped into a chunk its own pass had yet to run");
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0, count[1],
                                          "with the order built against the ray the count must move, or it "
                                          "is counting nothing");
}

/* The serial and two-core paths draw from genuinely different streams
 * now (sand_rng_next_at()), so this is not an equivalence check - it is
 * a sanity check that turning the switch on does not quietly turn it
 * into a no-op that happens to hash the same by never actually
 * splitting anything on a board with too few chunk rows. */
static void
test_two_core_step_actually_changes_the_draw_stream(void) {
    const uint32_t serial = tc_run_and_hash(3u, 40, false);
    const uint32_t two_core = tc_run_and_hash(3u, 40, true);

    TEST_ASSERT_NOT_EQUAL_MESSAGE(serial, two_core,
                                  "two-core stepping produced the same hash as serial on a "
                                  "board large enough to split - the chunk sweep "
                                  "should be drawing from sand_rng_next_at(), not silently "
                                  "falling back to the sequential stream");
}

/* The smallest grid the shipped step still splits. */
static void
test_two_core_step_changes_the_draw_stream_at_the_smallest_split_quality(void) {
    static const uint32_t seeds[] = {1u, 7u, 42u, 12345u, 99991u, 0xC0FFEEu};

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        const uint32_t serial = tc_run_quality_and_hash(92, 112, seeds[i], false);
        const uint32_t split = tc_run_quality_and_hash(92, 112, seeds[i], true);
        char why[160];
        snprintf(why, sizeof why, "92x112 seed %u: two-core stepping did not change the draw stream",
                 (unsigned)seeds[i]);
        TEST_ASSERT_NOT_EQUAL_MESSAGE(serial, split, why);
    }
}

/* Under SAND_CHUNK_SPLIT_MIN_CELLS the second core costs more than it buys,
 * so asking for it changes nothing - and a forced side still gets it, which
 * is what keeps the seam scenes on these grids worth running. */
static void
test_a_grid_under_the_split_floor_steps_serially_unless_a_side_is_forced(void) {
    static const struct {
        int w, h;
    } qualities[] = {{61, 74}, {46, 56}};

    static const uint32_t seeds[] = {1u, 7u, 42u, 12345u};

    for (size_t q = 0; q < sizeof qualities / sizeof qualities[0]; q++) {
        const int w = qualities[q].w;
        const int h = qualities[q].h;

        TEST_ASSERT_LESS_THAN_INT_MESSAGE(SAND_CHUNK_SPLIT_MIN_CELLS, w * h, "this grid must sit under the floor");
        for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
            const uint32_t serial = tc_run_quality_and_hash(w, h, seeds[i], false);
            char why[160];

            snprintf(why, sizeof why, "%dx%d seed %u: a grid under the floor split anyway", w, h, (unsigned)seeds[i]);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(serial, tc_run_quality_and_hash(w, h, seeds[i], true), why);

            const int side = tc_force_square_side(w, h);
            const uint32_t forced = tc_run_quality_and_hash(w, h, seeds[i], true);
            tc_release_square_side();
            snprintf(why, sizeof why, "%dx%d side %d seed %u: a forced side did not reach the split", w, h, side,
                     (unsigned)seeds[i]);
            TEST_ASSERT_NOT_EQUAL_MESSAGE(serial, forced, why);
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
    void* scratch = lane_scratch_open(&s);

    long before = 0;
    for (int i = 0; i < TC_W * TC_H; i++) {
        if (!CELL_IS_EMPTY(cells[i]) && CELL_MATERIAL(cells[i]) == MAT_WATER) {
            before += CELL_VARIANT(cells[i]);
        }
    }

    const unsigned swept_before = sand_split_dispatches[SAND_SPLIT_SLOT_SWEEP];
    const split_passes_scope_t split = split_passes_scope_begin(0u);
    sand_set_two_core_step(true);
    for (int i = 0; i < 60; i++) {
        sand_step(&s, 0, 1000, 0);
    }
    sand_set_two_core_step(false);
    split_passes_scope_end(split);
    tc_assert_split_arm_swept_split(swept_before, true);

    long after = 0;
    for (int i = 0; i < TC_W * TC_H; i++) {
        if (!CELL_IS_EMPTY(cells[i]) && CELL_MATERIAL(cells[i]) == MAT_WATER) {
            after += CELL_VARIANT(cells[i]);
        }
    }

    free(scratch);
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

/* True if row y sits within one row of a chunk boundary. */
static bool
row_near_chunk_boundary(int y, int side) {
    for (int m = -1; m <= 1; m++) {
        if (((y + m) % side) == 0) {
            return true;
        }
    }
    return false;
}

/* Folds occupied[]'s row-to-row deltas into the largest seen at a chunk
 * boundary vs anywhere in the interior - the tile-seam readout below. */
static void
worst_row_deltas(const int* occupied, int h, int side, int* interior_worst, int* boundary_worst) {
    for (int y = 1; y < h; y++) {
        const int delta = occupied[y] - occupied[y - 1];
        const int adelta = delta < 0 ? -delta : delta;
        if (row_near_chunk_boundary(y, side)) {
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
 * should follow the pile's own shape, not the chunk grid's - a tile
 * artifact would show up as a step in occupancy repeating every chunk side,
 * which a histogram of row-to-row deltas makes visible without eyeballing a
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
     * one-row margin of every chunk boundary - the pile's own surface,
     * which is not flat, sets this. */
    int interior_worst = 0;
    int boundary_worst = 0;
    worst_row_deltas(occupied, TC_H, sand_chunk_side_y(&s, SAND_CHUNK_TRAVEL_OTHER), &interior_worst, &boundary_worst);

    free(occupied);
    free(cells);
    free(blocks);

    char why[220];
    snprintf(why, sizeof why,
             "a settled pile's row-to-row occupancy jumped more at a chunk "
             "boundary (%d) than anywhere in the interior (%d) - that is "
             "what a baked-in tile seam looks like",
             boundary_worst, interior_worst);
    /* Generous on purpose: this is a statistical check on one seed, not a
     * pixel-exact one, and the pile's own surface already has real jumps
     * near its edges. The claim is only that a boundary is not a special,
     * repeatable outlier. */
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(interior_worst + TC_W / 10, boundary_worst, why);
}

/* A free-fall step advances the phase without touching a cell, so the next
 * real step sweeps in the other column order. */
static void
tc_prime_phase(sand_t* s, int phase) {
    if (phase == 0) {
        sand_step(s, 0, 0, 0);
    }
}

/* A lone-grain board as the app runs it: sleeping and step stamps on, and
 * scatter off so a fall draws no randomness. On the heap for the stack
 * ceiling above. */
typedef struct {
    sand_t s;
    uint8_t* cells;
    uint8_t* blocks;
    uint8_t* stamps;
    void* scratch;
} tc_board_t;

static tc_board_t*
tc_board_open(int w, int h, int phase) {
    tc_board_t* b = malloc(sizeof *b);
    TEST_ASSERT_NOT_NULL(b);
    b->cells = malloc((size_t)w * (size_t)h);
    b->blocks =
        malloc((size_t)((w + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * (size_t)((h + SAND_BLOCK_H - 1) / SAND_BLOCK_H));
    b->stamps = malloc(sand_step_stamp_bytes(w, h));
    TEST_ASSERT_NOT_NULL(b->cells);
    TEST_ASSERT_NOT_NULL(b->blocks);
    TEST_ASSERT_NOT_NULL(b->stamps);

    sand_init(&b->s, b->cells, w, h, 1u);
    sand_enable_sleeping(&b->s, b->blocks);
    sand_enable_step_stamps(&b->s, b->stamps);
    b->scratch = lane_scratch_open(&b->s);
    sand_set_scatter(&b->s, 0);
    tc_prime_phase(&b->s, phase);
    return b;
}

static void
tc_board_close(tc_board_t* b) {
    free(b->scratch);
    free(b->stamps);
    free(b->blocks);
    free(b->cells);
    free(b);
}

/* The seam and body scenes put one small shape on an otherwise empty board,
 * which is too little awake to be worth two lanes - so a split step here asks
 * for them by name and checks it was given them. A serial run would pass
 * every seam assertion below for the wrong reason. */
static void
tc_board_step(tc_board_t* b, int gx, int gy, bool two_core) {
    const sand_chunk_share_t share =
        sand_chunk_share_for_test(two_core ? SAND_CHUNK_SHARE_ALWAYS : SAND_CHUNK_SHARE_AUTO);
    const unsigned before = sand_split_dispatches[SAND_SPLIT_SLOT_SWEEP];

    sand_set_two_core_step(two_core);
    sand_step(&b->s, gx, gy, 0);
    sand_set_two_core_step(false);
    (void)sand_chunk_share_for_test(share);
    if (two_core) {
        TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(before, sand_split_dispatches[SAND_SPLIT_SLOT_SWEEP],
                                              "a split step swept on one core");
    }
}

/* Chebyshev distance from (x, y) to the board's only cell of `material`. */
static int
tc_grain_travel(const sand_t* s, int x, int y, material_id_t material) {
    int travel = -1;

    for (int gy = 0; gy < s->h; gy++) {
        for (int gx = 0; gx < s->w; gx++) {
            const cell_t c = sand_at(s, gx, gy);
            if (CELL_IS_EMPTY(c) || CELL_MATERIAL(c) != material) {
                continue;
            }
            TEST_ASSERT_EQUAL_INT_MESSAGE(-1, travel, "a lone grain must still be a lone grain after a step");
            const int ax = (gx > x) ? gx - x : x - gx;
            const int ay = (gy > y) ? gy - y : y - gy;
            travel = (ax > ay) ? ax : ay;
        }
    }
    return travel;
}

/* THE DOUBLE-MOVE CHECK: a lone grain with nothing to block it moves exactly
 * one cell in one step, as the serial sweep moves it. A grain that crosses
 * into a chunk whose own pass is still to come must not be picked up there
 * again - and at a corner it could otherwise be handed on twice. */
static void
tc_assert_free_fall_moves_one_cell(int phase, int gx, int gy, int start_x, int start_y, cell_t grain) {
    tc_board_t* b = tc_board_open(TC_W, TC_H, phase);
    sand_set(&b->s, start_x, start_y, grain);
    tc_board_step(b, gx, gy, true);
    const int travel = tc_grain_travel(&b->s, start_x, start_y, CELL_MATERIAL(grain));
    tc_board_close(b);

    char why[200];
    snprintf(why, sizeof why, "material %d at %d,%d (phase %d, gravity %d,%d) travelled %d cells in one split step",
             (int)CELL_MATERIAL(grain), start_x, start_y, phase, gx, gy, travel);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, travel, why);
}

/* A slide can move a grain diagonally even under axis gravity (see
 * Sand-Simulation.md's reach table), which is the other way a chunk can be
 * left - so this blocks the straight-ahead cell and holds the slide to the
 * same single cell. */
static void
tc_assert_forced_slide_moves_at_most_one_cell(int phase, int gx, int gy, int start_x, int start_y) {
    tc_board_t* b = tc_board_open(TC_W, TC_H, phase);
    int dx, dy;
    sand_gravity_direction(gx, gy, &dx, &dy);
    sand_set(&b->s, start_x + dx, start_y + dy, STONE);
    sand_set(&b->s, start_x, start_y, SAND);
    tc_board_step(b, gx, gy, true);
    const int travel = tc_grain_travel(&b->s, start_x, start_y, MAT_SAND);
    tc_board_close(b);

    char why[200];
    snprintf(why, sizeof why, "a lone grain at %d,%d (phase %d, gravity %d,%d) slid %d cells in one split step",
             start_x, start_y, phase, gx, gy, travel);
    TEST_ASSERT_TRUE_MESSAGE(travel >= 0, why);
    TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(1, travel, why);
}

static void
tc_assert_one_cell_at(int phase, int gx, int gy, int x, int y) {
    tc_assert_free_fall_moves_one_cell(phase, gx, gy, x, y, SAND);
    tc_assert_free_fall_moves_one_cell(phase, gx, gy, x, y, WATER);
    tc_assert_forced_slide_moves_at_most_one_cell(phase, gx, gy, x, y);
}

/* Both sides of every chunk-row boundary, with a row deep inside a chunk as
 * the control, under the four axis gravities. */
static void
tc_assert_row_seams_move_one_cell(int phase, int side) {
    for (int boundary = side; boundary < TC_H; boundary += side) {
        const int rows[] = {boundary - 2, boundary - 1, boundary, boundary + 1, boundary - side / 2};
        for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
            for (size_t g = 0; g < sizeof tc_ring / sizeof tc_ring[0]; g++) {
                if (tc_ring[g][0] == 0 || tc_ring[g][1] == 0) {
                    tc_assert_one_cell_at(phase, tc_ring[g][0] * 1000, tc_ring[g][1] * 1000, TC_W / 2, rows[r]);
                }
            }
        }
    }
}

/* Every cell within two of every interior chunk corner, under all eight
 * gravities: the worst case, where a grain could cross into one chunk still
 * to run and from it into another. */
static void
tc_assert_corners_move_one_cell(int phase, int side) {
    for (int cy = side; cy < TC_H; cy += side) {
        for (int cx = side; cx < TC_W; cx += side) {
            for (int oy = -2; oy < 2; oy++) {
                for (int ox = -2; ox < 2; ox++) {
                    for (size_t g = 0; g < sizeof tc_ring / sizeof tc_ring[0]; g++) {
                        tc_assert_one_cell_at(phase, tc_ring[g][0] * 1000, tc_ring[g][1] * 1000, cx + ox, cy + oy);
                    }
                }
            }
        }
    }
}

static void
test_two_core_step_never_double_moves_at_a_seam(void) {
    const int side = tc_force_square_side(TC_W, TC_H);

    for (int phase = 0; phase < 2; phase++) {
        tc_assert_row_seams_move_one_cell(phase, side);
        tc_assert_corners_move_one_cell(phase, side);
    }
    tc_release_square_side();
}

/* THE CROSSING-ARRIVAL CHECK: a liquid moving with gravity lands in a chunk
 * the schedule swept first, so its new block's own BLOCK_HAS_LIQUID is a step
 * behind. Cross-flow's gate is BLOCK_LIQUID_NEAR, which the block it left -
 * an 8-neighbour of this one - covers. The move has to leave the cell's own
 * block too, or that mark is the departure's own. */
static void
tc_assert_a_crossing_liquid_marks_where_it_lands(int gx, int gy, int start_x, int start_y, int dest_x, int dest_y) {
    tc_board_t* b = tc_board_open(TC_W, TC_H, 1);
    const int cols = b->s.block_cols;
    const int started = (start_y / SAND_BLOCK_H) * cols + start_x / SAND_BLOCK_W;
    const int landed = (dest_y / SAND_BLOCK_H) * cols + dest_x / SAND_BLOCK_W;

    sand_set(&b->s, start_x, start_y, WATER);
    tc_board_step(b, gx, gy, true);

    const cell_t arrived = sand_at(&b->s, dest_x, dest_y);
    const uint8_t state = b->s.block_state[landed];

    for (int i = 0; i < 4; i++) {
        tc_board_step(b, gx, gy, true);
    }
    const bool still_liquid = b->s.may_have_liquid;
    tc_board_close(b);

    char why[200];
    snprintf(why, sizeof why, "water %d,%d -> %d,%d under gravity %d,%d left its new block at state %02x", start_x,
             start_y, dest_x, dest_y, gx, gy, (unsigned)state);
    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE(started, landed, why);
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(arrived), why);
    TEST_ASSERT_TRUE_MESSAGE((state & BLOCK_LIQUID_NEAR) != 0, why);
    TEST_ASSERT_TRUE_MESSAGE(still_liquid, why);
}

/* The crossing has to leave the cell's own block as well as its chunk, so
 * this picks a side whose borders land on block borders rather than taking
 * whatever the shipped rule gives - which need not divide by either. */
static void
test_a_liquid_crossing_a_chunk_border_stays_in_cross_flow_reach(void) {
    const int side = 2 * SAND_BLOCK_H; /* a multiple of both block axes */
    const int x = (TC_W / 2 / SAND_BLOCK_W) * SAND_BLOCK_W + SAND_BLOCK_W / 2;
    const int y = (side / 2 / SAND_BLOCK_H) * SAND_BLOCK_H + SAND_BLOCK_H / 2;

    TEST_ASSERT_TRUE_MESSAGE(sand_chunk_side_for_test(side, side), "the crossing side must clear the chunk floor");
    tc_assert_a_crossing_liquid_marks_where_it_lands(0, 1000, x, side - 1, x, side);
    tc_assert_a_crossing_liquid_marks_where_it_lands(1000, 0, side - 1, y, side, y);
    (void)sand_chunk_side_for_test(0, 0);
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
            void* scratch = lane_scratch_open(&two_core_s);
            sand_set_scatter(&serial_s, 0);
            sand_set_scatter(&two_core_s, 0);
            tc_prime_phase(&serial_s, offset);
            tc_prime_phase(&two_core_s, offset);

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

            free(scratch);
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

/* A zero-randomness lone grain stepped serially and split on the same board.
 * Both boards carry stamps; the serial step must not read them. */
static void
tc_assert_quality_fall_matches_serial(int w, int h, int phase, int x, int y, int gx, int gy) {
    tc_board_t* serial = tc_board_open(w, h, phase);
    tc_board_t* split = tc_board_open(w, h, phase);
    sand_set(&serial->s, x, y, SAND);
    sand_set(&split->s, x, y, SAND);

    tc_board_step(serial, gx, gy, false);
    tc_board_step(split, gx, gy, true);

    const uint32_t serial_hash = tc_hash(serial->cells, (size_t)w * (size_t)h);
    const uint32_t split_hash = tc_hash(split->cells, (size_t)w * (size_t)h);
    tc_board_close(serial);
    tc_board_close(split);

    char why[160];
    snprintf(why, sizeof why, "%dx%d phase %d grain %d,%d gravity %d,%d: a seam fall differed from serial", w, h, phase,
             x, y, gx, gy);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(serial_hash, split_hash, why);
}

static void
tc_assert_quality_row_seams_match_serial(int w, int h, int side, int phase) {
    for (int boundary = side; boundary < h; boundary += side) {
        for (int y = boundary - 2; y <= boundary + 1 && y < h; y++) {
            tc_assert_quality_fall_matches_serial(w, h, phase, w / 2, y, 0, 1000);
            tc_assert_quality_fall_matches_serial(w, h, phase, w / 2, y, 0, -1000);
        }
    }
}

static void
tc_assert_quality_corners_match_serial(int w, int h, int side, int phase) {
    for (int cy = side; cy < h; cy += side) {
        for (int cx = side; cx < w; cx += side) {
            for (int oy = -2; oy < 2; oy++) {
                for (int ox = -2; ox < 2; ox++) {
                    for (size_t g = 0; g < sizeof tc_ring / sizeof tc_ring[0]; g++) {
                        tc_assert_quality_fall_matches_serial(w, h, phase, cx + ox, cy + oy, tc_ring[g][0] * 1000,
                                                              tc_ring[g][1] * 1000);
                    }
                }
            }
        }
    }
}

/* Every chunk seam and corner at every smaller quality the app offers must
 * fall exactly as serial - where a grain leaves its chunk as much as where it
 * stays inside one. */
static void
test_smaller_quality_seams_match_serial(void) {
    static const struct {
        int w, h;
    } qualities[] = {{92, 112}, {61, 74}, {46, 56}};

    for (size_t q = 0; q < sizeof qualities / sizeof qualities[0]; q++) {
        const int side = tc_force_square_side(qualities[q].w, qualities[q].h);
        for (int phase = 0; phase < 2; phase++) {
            tc_assert_quality_row_seams_match_serial(qualities[q].w, qualities[q].h, side, phase);
            tc_assert_quality_corners_match_serial(qualities[q].w, qualities[q].h, side, phase);
        }
        tc_release_square_side();
    }
}

#define TC_BODY_SIDE      48
#define TC_BODY_STEPS_MAX 40

/* A solid square straddling the first chunk border on both axes, so it is
 * astride one for every step it can travel. */
static void
tc_build_solid_body(sand_t* s, int side, cell_t fill, int* out_x0, int* out_y0) {
    const int x0 = side - TC_BODY_SIDE / 2;
    const int y0 = side - TC_BODY_SIDE / 2;

    for (int y = y0; y < y0 + TC_BODY_SIDE; y++) {
        for (int x = x0; x < x0 + TC_BODY_SIDE; x++) {
            sand_set(s, x, y, fill);
        }
    }
    *out_x0 = x0;
    *out_y0 = y0;
}

/* How far the body can travel before an edge of it reaches a wall, where it
 * would stop being a body in free fall. */
static int
tc_body_steps(int x0, int y0, int tx, int ty) {
    const int room_x = (tx > 0) ? TC_W - 1 - (x0 + TC_BODY_SIDE) : (tx < 0) ? x0 - 1 : TC_BODY_STEPS_MAX;
    const int room_y = (ty > 0) ? TC_H - 1 - (y0 + TC_BODY_SIDE) : (ty < 0) ? y0 - 1 : TC_BODY_STEPS_MAX;
    const int room = (room_x < room_y) ? room_x : room_y;

    return (room < TC_BODY_STEPS_MAX) ? room : TC_BODY_STEPS_MAX;
}

typedef struct {
    int count, x0, y0, x1, y1;
} tc_extent_t;

static void
tc_extent_add(tc_extent_t* e, int x, int y) {
    e->count++;
    e->x0 = (x < e->x0) ? x : e->x0;
    e->y0 = (y < e->y0) ? y : e->y0;
    e->x1 = (x > e->x1) ? x : e->x1;
    e->y1 = (y > e->y1) ? y : e->y1;
}

/* Still solid exactly when the cells fill their own bounding box: a gap
 * opened on a chunk border stretches that box without adding a cell. */
static bool
tc_body_is_solid(const sand_t* s, material_id_t material) {
    tc_extent_t e = {0, s->w, s->h, -1, -1};

    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            const cell_t c = sand_at(s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == material) {
                tc_extent_add(&e, x, y);
            }
        }
    }
    return e.count == TC_BODY_SIDE * TC_BODY_SIDE && e.x1 - e.x0 + 1 == TC_BODY_SIDE && e.y1 - e.y0 + 1 == TC_BODY_SIDE;
}

/* The step at which the body first stopped being one, or -1. */
static int
tc_body_breaks_at(cell_t fill, material_id_t material, int tx, int ty, bool two_core) {
    const int side = tc_force_square_side(TC_W, TC_H);
    tc_board_t* b = tc_board_open(TC_W, TC_H, 1);
    int x0, y0, broke = -1;

    tc_build_solid_body(&b->s, side, fill, &x0, &y0);
    const int steps = tc_body_steps(x0, y0, tx, ty);
    for (int i = 0; i < steps && broke < 0; i++) {
        tc_board_step(b, tx * 1000, ty * 1000, two_core);
        if (!tc_body_is_solid(&b->s, material)) {
            broke = i;
        }
    }
    tc_board_close(b);
    tc_release_square_side();
    return broke;
}

/* Every direction in one verdict, since which ones break is the evidence:
 * a fixed pass order holds against travel for half of them and runs the
 * upstream chunk of a border first for the other half. */
static void
tc_assert_no_direction_breaks_a_body(cell_t fill, material_id_t material, bool two_core, const char* what) {
    char broke[128] = "";
    size_t used = 0;

    for (size_t g = 0; g < sizeof tc_ring / sizeof tc_ring[0]; g++) {
        const int at = tc_body_breaks_at(fill, material, tc_ring[g][0], tc_ring[g][1], two_core);
        if (at >= 0 && used < sizeof broke - 1) {
            used += (size_t)snprintf(broke + used, sizeof broke - used, " %d,%d@%d", tc_ring[g][0], tc_ring[g][1], at);
        }
    }

    char why[200];
    snprintf(why, sizeof why, "%s opened a gap under gravity:%s", what, broke);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)broke[0], why);
}

static void
test_a_split_falling_body_of_sand_opens_no_gap(void) {
    tc_assert_no_direction_breaks_a_body(SAND, MAT_SAND, true, "a split sand body");
}

static void
test_a_serial_falling_body_of_sand_opens_no_gap(void) {
    tc_assert_no_direction_breaks_a_body(SAND, MAT_SAND, false, "a serial sand body");
}

static void
test_a_split_falling_body_of_water_opens_no_gap(void) {
    tc_assert_no_direction_breaks_a_body(WATER, MAT_WATER, true, "a split water body");
}

static void
test_a_serial_falling_body_of_water_opens_no_gap(void) {
    tc_assert_no_direction_breaks_a_body(WATER, MAT_WATER, false, "a serial water body");
}

/* A gas body cannot be asked to stay one: a walk draws sideways and downward
 * too, so it frays whoever steps it. A shaft of stone one cell wide takes
 * those draws away, leaving the falling body's own claim - a packed run
 * advances as a chain, not leaving a hole where a border was. Its gas all
 * sits in one chunk line, so the schedule walks it in serial's own order.
 *
 * A rise is one draw in four, so the border only comes into it when the
 * leading grain and the one behind both draw one. */
#define TC_SHAFT_PHASES 32

/* sand_force_hashed_rng() reaches the serial sweep, not this pass, and a
 * split pass arms the hashed draws for itself - so the serial side is armed
 * here or the two roll different walks and never had an order to compare. */
static void
tc_step_gas_under(sand_t* s, int dx, int dy, bool two_core) {
    const int ring = ring_of(dx, dy);
    const int* const slide_a = ring_dir(ring + 7);
    const int* const slide_b = ring_dir(ring + 1);
    const int* const perp_a = ring_dir(ring + 2);
    const int* const perp_b = ring_dir(ring + 6);

    /* sweep_x_order()'s answer for this pull, which the gas pass negates. */
    const int x_step = (dx > 0) ? -1 : 1;

    sand_set_two_core_step(two_core);
    s->rng_hashed = true;
    sand_step_gas(s, dx * 1000, dy * 1000, dx, dy, slide_a, slide_b, perp_a, perp_b, dx, dy, x_step, 0);
    s->rng_hashed = false;
    sand_set_two_core_step(false);
}

/* One cell in the shaft's own frame - down its length, and across it. */
static void
tc_shaft_set(sand_t* s, bool vertical, int along, int across, cell_t c) {
    sand_set(s, vertical ? across : along, vertical ? along : across, c);
}

/* Walls either side of the shaft; gas packs one chunk's worth of it against
 * the closed gravity-ward end, so the run straddles a border and has open
 * shaft only ahead of it. Packed and floored, the step's every legal draw is
 * the straight rise, which is what makes one step comparable at all: a draw
 * landing downstream moves twice for serial and once here. The leading grain
 * sits one cell short of a border, since only the two or three behind it
 * ever get to move. */
static void
tc_build_gas_shaft(sand_t* s, int side, int dx, int dy) {
    const bool vertical = (dy != 0);
    const int span = vertical ? TC_H : TC_W;
    const int across = side + side / 2;
    const int rise = -(vertical ? dy : dx);
    const int lead = (rise < 0) ? 2 * side - 1 : 2 * side;
    const int from = (rise < 0) ? lead : 0;
    const int to = (rise < 0) ? span - 1 : lead;

    for (int along = 0; along < span; along++) {
        tc_shaft_set(s, vertical, along, across - 1, STONE);
        tc_shaft_set(s, vertical, along, across + 1, STONE);
    }
    for (int along = from; along <= to; along++) {
        tc_shaft_set(s, vertical, along, across, GAS);
    }
}

static bool
tc_gas_shaft_matches_serial(int side, int dx, int dy, int phase) {
    tc_board_t* serial = tc_board_open(TC_W, TC_H, 1);
    tc_board_t* split = tc_board_open(TC_W, TC_H, 1);

    for (int i = 0; i < 2; i++) {
        tc_board_t* b = (i == 0) ? serial : split;
        sand_set_decay(&b->s, 0);
        tc_build_gas_shaft(&b->s, side, dx, dy);
        b->s.step_phase = (uint16_t)phase;
    }

    tc_step_gas_under(&split->s, dx, dy, true);
    tc_step_gas_under(&serial->s, dx, dy, false);

    const bool match = memcmp(split->cells, serial->cells, (size_t)TC_W * (size_t)TC_H) == 0;
    tc_board_close(serial);
    tc_board_close(split);
    return match;
}

/* Every pull in one verdict, since which ones part is the evidence: a fixed
 * pass order holds against the rise for half of them and runs the upstream
 * chunk of a border first for the other half. Only the four axis pulls - a
 * diagonal rise has no straight shaft to take the sideways draws away. */
static void
test_a_split_rising_body_of_gas_opens_no_gap(void) {
    static const int pulls[][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
    const int side = tc_force_square_side(TC_W, TC_H);
    char parted[128] = "";
    size_t used = 0;

    /* The run has to cross a border, and its shaft has to clear the walls. */
    TEST_ASSERT_TRUE(2 * side < TC_W);
    TEST_ASSERT_TRUE(side + side / 2 + 1 < TC_H);

    for (size_t i = 0; i < sizeof pulls / sizeof pulls[0]; i++) {
        for (int phase = 0; phase < TC_SHAFT_PHASES; phase++) {
            if (!tc_gas_shaft_matches_serial(side, pulls[i][0], pulls[i][1], phase) && used < sizeof parted - 1) {
                used +=
                    (size_t)snprintf(parted + used, sizeof parted - used, " %d,%d@%d", pulls[i][0], pulls[i][1], phase);
            }
        }
    }

    tc_release_square_side();

    char why[200];
    snprintf(why, sizeof why, "a split gas shaft left a cell serial did not:%s", parted);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)parted[0], why);
}

#define TC_FUSE_IMPULSE_MAX 2048

/* A lit 2x2 of gunpowder, walled so it cannot fall apart before a corner
 * burns out - the scene suite_sand_gunpowder.c uses for the blast rule.
 * The split defers the blast to the serial reach pass, and the cell that
 * queued it has burned out by the time that pass runs, so a reach that asks
 * the board what the cell explodes for finds nothing and drops it. */
static int
tc_fuse_blast_impulses(bool two_core) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    TEST_ASSERT_NOT_NULL(cells);
    /* One blast's worth, not one entry per cell: a grid-sized queue is a
     * quarter megabyte and the host arena models the device heap. */
    impulse_t* buf = malloc((size_t)TC_FUSE_IMPULSE_MAX * sizeof *buf);
    TEST_ASSERT_NOT_NULL(buf);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 5u);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    sand_enable_impulses(&s, buf, TC_FUSE_IMPULSE_MAX);

    for (int x = 0; x < TC_W; x++) {
        sand_set(&s, x, TC_H - 1, STONE);
    }
    sand_set(&s, 2, TC_H - 2, STONE);
    sand_set(&s, 5, TC_H - 2, STONE);
    sand_set(&s, 2, TC_H - 3, STONE);
    sand_set(&s, 5, TC_H - 3, STONE);
    sand_set(&s, 3, TC_H - 3, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, TC_H - 3, GUNPOWDER_LIT_CELL);
    sand_set(&s, 3, TC_H - 2, GUNPOWDER_LIT_CELL);
    sand_set(&s, 4, TC_H - 2, GUNPOWDER_LIT_CELL);

    sand_set_two_core_step(two_core);
    bool burned = false;
    for (int i = 0; i < 200 && !burned; i++) {
        sand_step(&s, 0, 1000, 0);
        burned = !cell_is_gunpowder(sand_at(&s, 3, TC_H - 3)) || !cell_is_gunpowder(sand_at(&s, 4, TC_H - 3))
                 || !cell_is_gunpowder(sand_at(&s, 3, TC_H - 2)) || !cell_is_gunpowder(sand_at(&s, 4, TC_H - 2));
    }
    const int impulses = s.impulse_count;
    sand_set_two_core_step(false);

    free(cells);
    free(buf);
    TEST_ASSERT_TRUE_MESSAGE(burned, "setup: a corner of the lit 2x2 must burn out within the budget");
    return impulses;
}

/* Lava sealed under stone, with the burst forced certain: covered_at() is
 * what a burst needs, and the roll must happen per cell. A split that queues
 * every burning lava cell and rolls later gets only as many chances as the
 * queue is deep, which a screen of lava exhausts immediately. */
static int
tc_lava_burst_impulses(bool two_core) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    TEST_ASSERT_NOT_NULL(cells);
    impulse_t* buf = malloc((size_t)TC_FUSE_IMPULSE_MAX * sizeof *buf);
    TEST_ASSERT_NOT_NULL(buf);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 9u);
    sand_set_decay(&s, SAND_DECAY_PER_MATERIAL);
    /* The NATURAL burst chance on purpose: forcing it certain would make
     * every candidate a winner and hide the thing this guards, which is that
     * a rare roll must be offered to every lava cell rather than to however
     * many fit in the deferred queue. */
    sand_enable_impulses(&s, buf, TC_FUSE_IMPULSE_MAX);

    for (int y = TC_H - 40; y < TC_H; y++) {
        for (int x = 0; x < TC_W; x++) {
            sand_set(&s, x, y, STONE);
        }
    }
    for (int y = TC_H - 36; y < TC_H - 6; y++) {
        for (int x = 4; x < TC_W - 4; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_LAVA, MASS_MAX));
        }
    }

    sand_set_two_core_step(two_core);
    int peak = 0;
    for (int i = 0; i < 20; i++) {
        sand_step(&s, 0, 1000, 0);
        if (s.impulse_count > peak) {
            peak = s.impulse_count;
        }
    }
    sand_set_two_core_step(false);

    free(cells);
    free(buf);
    return peak;
}

static void
test_a_lava_burst_throws_grains_on_both_cores(void) {
    const int serial = tc_lava_burst_impulses(false);
    const int split = tc_lava_burst_impulses(true);

    char why[160];
    snprintf(why, sizeof why,
             "serial threw %d grains and the split threw %d: a confined lava burst must survive "
             "the reach pass",
             serial, split);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, serial, why);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, split, why);
}

static void
test_a_fuse_blast_throws_grains_on_both_cores(void) {
    const int serial = tc_fuse_blast_impulses(false);
    const int split = tc_fuse_blast_impulses(true);

    char why[160];
    snprintf(why, sizeof why,
             "serial threw %d grains and the split threw %d: a deferred blast must survive the "
             "reach pass",
             serial, split);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, serial, why);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, split, why);
}

static void
test_a_settled_chunk_does_no_row_work(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 1u);
    sand_enable_sleeping(&s, blocks);
    void* scratch = lane_scratch_open(&s);
    for (int y = 0; y < TC_H; y++) {
        for (int x = 0; x < TC_W; x++) {
            sand_set(&s, x, y, STONE);
        }
    }

    sand_set_two_core_step(true);
    sand_step(&s, 0, 1000, 0);
    sand_sweep_chunks_swept = 0;
    sand_step(&s, 0, 1000, 0);
    sand_set_two_core_step(false);

    free(scratch);
    free(cells);
    free(blocks);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, sand_sweep_chunks_swept,
                                   "a chunk whose blocks have all settled must be skipped before any row work");
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

/* A single, unbroken column of touching grains spanning every chunk row in
 * the grid - unlike the lone grain above, each cell's upstream neighbour is
 * occupied too, so a boundary here starts genuinely contested rather than
 * empty. */
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
    void* scratch = lane_scratch_open(&s);
    sand_set_scatter(&s, 0);
    tc_prime_phase(&s, offset);
    build(&s);

    sand_set_two_core_step(two_core);
    for (int i = 0; i < steps; i++) {
        sand_step(&s, gx, gy, 0);
    }
    sand_set_two_core_step(false);
    tc_collect_core1();

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

    free(scratch);
    free(cells);
    free(blocks);
    return h;
}

/* A dense column and pile, where a boundary cell's neighbour is never
 * empty, so a move there truly contends with something. Not a hash-identity
 * check: no fixed pass order can reproduce serial order across a boundary -
 * see "What a pass boundary still costs" in Sand-Simulation.md. This checks
 * the part that must still hold - the grain count - which a double-move or
 * a dropped cell would break. */
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

/* ACROSS gravity, which under a landscape pull means a column: along it, a
 * fall moves mass within one line and a chunk border that stalled would
 * leave no trace at all. */
static int
tc_water_column_mass(const sand_t* s, int x) {
    int mass = 0;
    for (int y = 0; y < TC_H; y++) {
        const cell_t c = sand_at(s, x, y);
        if (CELL_MATERIAL(c) == MAT_WATER) {
            mass += CELL_VARIANT(c);
        }
    }
    return mass;
}

static void
test_landscape_water_column_has_no_line_mass_lag(void) {
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
    void* scratch = lane_scratch_open(&split);
    for (int y = 0; y < TC_H / 2; y++) {
        for (int x = TC_W / 2 - 6; x < TC_W / 2 + 6; x++) {
            sand_set(&serial, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
            sand_set(&split, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    int worst_line_mass = 0;
    sand_force_hashed_rng(true);
    for (int step = 0; step < 40; step++) {
        memcpy(split_cells, serial_cells, (size_t)TC_W * (size_t)TC_H);
        memcpy(split_blocks, serial_blocks, (size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
        split.step_phase = serial.step_phase;

        sand_set_two_core_step(true);
        sand_step(&split, 1000, 0, 0);
        sand_set_two_core_step(false);
        sand_step(&serial, 1000, 0, 0);

        for (int x = 0; x < TC_W; x++) {
            const int difference = tc_water_column_mass(&split, x) - tc_water_column_mass(&serial, x);
            const int line_mass = difference < 0 ? -difference : difference;
            if (line_mass > worst_line_mass) {
                worst_line_mass = line_mass;
            }
        }
    }
    sand_force_hashed_rng(false);

    free(scratch);
    free(serial_cells);
    free(split_cells);
    free(serial_blocks);
    free(split_blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, worst_line_mass,
                                  "the split landscape water column left liquid mass in a different column "
                                  "than serial");
}

static int
tc_first_gas_equalise_boundary(const sand_t* s) {
    return sand_chunk_side_y(s, SAND_CHUNK_TRAVEL_OTHER);
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
    sand_gas_equalise_runs = 0;
    for (int phase = 0; phase < TC_SEAM_PHASES; phase++) {
        uint8_t* serial_cells = malloc((size_t)TC_W * (size_t)TC_H);
        uint8_t* split_cells = malloc((size_t)TC_W * (size_t)TC_H);
        TEST_ASSERT_NOT_NULL(serial_cells);
        TEST_ASSERT_NOT_NULL(split_cells);

        sand_t serial, split;
        sand_init(&serial, serial_cells, TC_W, TC_H, 1u);
        sand_init(&split, split_cells, TC_W, TC_H, 1u);
        void* scratch = lane_scratch_open(&split);
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
        free(scratch);
        free(serial_cells);
        free(split_cells);
        TEST_ASSERT_TRUE_MESSAGE(cells_match, why);
    }
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0, sand_gas_equalise_runs,
                                          "portrait gas equalise did not enter its split passes");
}

/* A lone gas grain under a ceiling, which the walk leaves alone (mobility 0)
 * and the spread hops one cell sideways. */
static void
tc_assert_gas_hop_matches_serial(int x, int y, bool flip) {
    tc_board_t* serial = tc_board_open(TC_W, TC_H, 1);
    tc_board_t* split = tc_board_open(TC_W, TC_H, 1);
    tc_board_t* boards[] = {serial, split};
    for (int i = 0; i < 2; i++) {
        sand_set_mobility(&boards[i]->s, 0);
        for (int cx = 0; cx < TC_W; cx++) {
            sand_set(&boards[i]->s, cx, y - 1, STONE);
        }
        sand_set(&boards[i]->s, x, y, GAS);
        boards[i]->s.gas_flip = flip;
    }

    tc_step_gas_equalise(&serial->s, false);
    tc_step_gas_equalise(&split->s, true);

    const bool cells_match = memcmp(serial->cells, split->cells, (size_t)TC_W * (size_t)TC_H) == 0;
    tc_board_close(serial);
    tc_board_close(split);

    char why[160];
    snprintf(why, sizeof why, "gas at %d,%d (flip %d): a split spread hop differed from serial", x, y, (int)flip);
    TEST_ASSERT_TRUE_MESSAGE(cells_match, why);
}

/* A hop that lands in a chunk whose own pass is still to come must not be
 * taken again there. Both sides of every chunk column seam, both ways. */
static void
test_split_gas_equalise_hops_once_across_a_chunk_column(void) {
    const int side = tc_force_square_side(TC_W, TC_H);

    for (int boundary = side; boundary < TC_W; boundary += side) {
        for (int x = boundary - 1; x <= boundary; x++) {
            tc_assert_gas_hop_matches_serial(x, side / 2, false);
            tc_assert_gas_hop_matches_serial(x, side / 2, true);
        }
    }
    tc_release_square_side();
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

/* THE REACTION SPLIT - sand_step_reactions() called directly, never through
 * sand_step(), so these tests are about the local-rule split alone and
 * cannot be confused with the sweep or gas walk's own coverage above. */

/* Isolated fire/gas pairs, never two gas cells touching. GAS ignites free
 * of randomness (material.c) - but a CONNECTED pocket is not a fair split
 * vs serial comparison regardless: a cell ignited ahead of the scan
 * spreads further within the step, and chunk order is not row-major. */
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
    void* scratch = lane_scratch_open(&s);

    sand_set_two_core_step(two_core);
    for (int i = 0; i < steps; i++) {
        sand_step_reactions(&s);
    }
    sand_set_two_core_step(false);
    tc_collect_core1();

    const uint32_t hash = tc_hash(cells, (size_t)w * (size_t)h);
    free(scratch);
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

/* A scattered mix of every stage the split touches - burning oil and gas,
 * conducting/heat-ramped stone and glass, acid dissolving, a lava/water
 * cool-off pair - so real chance rolls happen throughout, unlike the
 * zero-randomness scene above. No plant and nothing that becomes one: a
 * grower or a drinker on the board turns the split off, so a scene holding
 * one would measure the serial walk instead. */
static void
rc_build_reaction_heavy_scene(sand_t* s, uint8_t* cells, int w, int h, uint32_t seed) {
    sand_init(s, cells, w, h, seed);

    rng_t r;
    rng_seed(&r, seed ^ 0x51ED5EEDu);
    static const cell_t picks[] = {STONE, GLASS, OIL, GAS, WATER, LAVA, ACID};
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

/* Block sleeping and dirty rows are on because they are what a lane keeps
 * privately: with them off the hash could not tell a lane's own shadow from
 * one shared with the other core. */
static uint32_t
rc_run_reaction_heavy_and_hash(int w, int h, uint32_t seed, int steps, bool two_core) {
    const size_t blocks =
        (size_t)((w + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * (size_t)((h + SAND_BLOCK_H - 1) / SAND_BLOCK_H);
    uint8_t* cells = malloc((size_t)w * (size_t)h);
    uint8_t* block_state = malloc(blocks);
    uint8_t* rows = malloc((size_t)h);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(block_state);
    TEST_ASSERT_NOT_NULL(rows);

    sand_t s;
    rc_build_reaction_heavy_scene(&s, cells, w, h, seed);
    sand_enable_sleeping(&s, block_state);
    sand_track_dirty_rows(&s, rows);
    void* scratch = lane_scratch_open(&s);

    /* Gravity turns under the pass without the sweep running: a reaction
     * emits, percolates and reads its lid along the settled direction, so a
     * fixed one would only ever exercise one of them. */
    static const int8_t down[][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}, {1, 1}};

    sand_set_two_core_step(two_core);
    for (int i = 0; i < steps; i++) {
        const int arm = i % (int)(sizeof down / sizeof down[0]);
        s.last_load_dx = down[arm][0];
        s.last_load_dy = down[arm][1];
        s.last_step_dx = down[arm][0];
        s.last_step_dy = down[arm][1];
        s.step_phase = (uint16_t)i;
        sand_step_reactions(&s);
    }
    sand_set_two_core_step(false);
    tc_collect_core1();

    uint32_t hash = tc_hash(cells, (size_t)w * (size_t)h);
    hash ^= tc_hash(block_state, blocks) * 0x9E3779B1u;
    hash ^= tc_hash(rows, (size_t)h) * 0x85EBCA6Bu;
    free(scratch);
    free(rows);
    free(block_state);
    free(cells);
    return hash;
}

/* Every chunk of the board with deferred work to hand over in one step: a
 * lava grain beside water queues a cool-off chain the moment it quenches,
 * and cold glass beside fire queues a crack run on first contact. Spaced so
 * neither lane's queue fills - a full one drops candidates, and then nothing
 * could be counted. */
#define RC_STORM_SPACING 14

static void
rc_paint_deferred_storm(sand_t* s) {
    for (int y = 2; y < TC_H - 6; y += RC_STORM_SPACING) {
        for (int x = 2; x < TC_W - 2; x += RC_STORM_SPACING) {
            sand_set(s, x, y, LAVA);
            sand_set(s, x + 1, y, WATER);
        }
    }
    /* The crack queue is half the cool-off queue's depth, so its pairs are
     * spread twice as thin to keep both lanes the same distance from full. */
    for (int y = 6; y < TC_H - 2; y += 2 * RC_STORM_SPACING) {
        for (int x = 2; x < TC_W - 2; x += 2 * RC_STORM_SPACING) {
            sand_set(s, x, y, CELL_MAKE(MAT_GLASS, 0));
            sand_set(s, x + 1, y, FIRE);
        }
    }
}

/* A board starts pessimistic about what it holds, so its first reaction step
 * always walks serially; the scene is repainted on a cleared board for the
 * split step this measures. */
static uint32_t
rc_run_deferred_storm_and_hash(sand_chunk_pass_driver_t driver) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 9u);
    void* scratch = lane_scratch_open(&s);
    rc_paint_deferred_storm(&s);

    sand_set_two_core_step(true);
    sand_chunk_pass_set_driver_for_test(driver);
    sand_step_reactions(&s);
    memset(cells, CELL_EMPTY, (size_t)TC_W * (size_t)TC_H);
    rc_paint_deferred_storm(&s);
    sand_step_reactions(&s);
    sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);
    sand_set_two_core_step(false);
    tc_collect_core1();

    const uint32_t hash = tc_hash(cells, (size_t)TC_W * (size_t)TC_H);
    free(scratch);
    free(cells);
    return hash;
}

/* The claim the per-lane queues rest on: both lanes really do queue, no
 * entry is lost or run twice, and no queue reached its cap - past the cap
 * the pass drops candidates and the count would no longer mean anything. */
static void
test_every_deferred_reaction_effect_is_applied_exactly_once(void) {
    sand_reactions_defer_queued[0] = 0;
    sand_reactions_defer_queued[1] = 0;
    sand_reactions_defer_applied = 0;
    sand_reactions_defer_peak_q8 = 0;

    const uint32_t solo = rc_run_deferred_storm_and_hash(SAND_CHUNK_PASS_SOLO);
    const unsigned queued = sand_reactions_defer_queued[0] + sand_reactions_defer_queued[1];

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0, sand_reactions_defer_queued[0], "lane 0 deferred nothing to stress");
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0, sand_reactions_defer_queued[1], "lane 1 deferred nothing to stress");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(queued, sand_reactions_defer_applied,
                                   "a deferred reaction effect was lost or applied twice");
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(128u, sand_reactions_defer_peak_q8,
                                          "no queue got half full, so this scene is not stressing them");
    TEST_ASSERT_LESS_THAN_UINT_MESSAGE(256u, sand_reactions_defer_peak_q8,
                                       "a lane's queue filled, so the drop policy - not the drain - decided the board");

    static const sand_chunk_pass_driver_t drivers[TC_DRIVERS] = {
        SAND_CHUNK_PASS_LANE0_EAGER, SAND_CHUNK_PASS_LANE1_EAGER, SAND_CHUNK_PASS_ALTERNATE};
    for (int d = 0; d < TC_DRIVERS; d++) {
        sand_reactions_defer_queued[0] = 0;
        sand_reactions_defer_queued[1] = 0;
        sand_reactions_defer_applied = 0;
        const uint32_t driven = rc_run_deferred_storm_and_hash(drivers[d]);
        char why[160];
        snprintf(why, sizeof why, "driver %d: the deferred storm's board depended on the lane interleaving",
                 (int)drivers[d]);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(solo, driven, why);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(queued, sand_reactions_defer_queued[0] + sand_reactions_defer_queued[1], why);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(queued, sand_reactions_defer_applied, why);
    }
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

#ifdef DEVICE_BUILD
/* The same claim as the queue twin below, on the scene where every stage the
 * split touches runs with real chance rolls rather than a fixed chain. */
static void
test_a_core_1_reaction_lane_lands_on_the_solo_heavy_board(void) {
    static const uint32_t seeds[] = {1u, 7u, 42u};

    TEST_ASSERT_TRUE_MESSAGE(job_try_core1(tc_no_op_job, NULL, 0), "no core-1 worker to compare against");
    TEST_ASSERT_TRUE(job_wait(100));

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);
        const uint32_t duo = rc_run_reaction_heavy_and_hash(TC_W, TC_H, seeds[i], 30, true);
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_SOLO);
        const uint32_t solo = rc_run_reaction_heavy_and_hash(TC_W, TC_H, seeds[i], 30, true);
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);

        char why[160];
        snprintf(why, sizeof why, "seed %u: two cores and the single-thread walk parted on a reacting board",
                 (unsigned)seeds[i]);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(solo, duo, why);
    }
}

/* The twin of test_a_core_1_lane_lands_on_the_solo_board(), for the pass
 * whose lanes hand work to each other through queues rather than through
 * cells: both lanes fill a queue, and the drain after the join must not be
 * able to tell which core filled which. Every interleaving a host can stage
 * is a guess at this; only a real core answers it. */
static void
test_a_core_1_reaction_lane_lands_on_the_solo_board(void) {
    TEST_ASSERT_TRUE_MESSAGE(job_try_core1(tc_no_op_job, NULL, 0), "no core-1 worker to compare against");
    TEST_ASSERT_TRUE(job_wait(100));

    for (int round = 0; round < 3; round++) {
        const uint32_t duo = rc_run_deferred_storm_and_hash(SAND_CHUNK_PASS_CORE1);
        const uint32_t solo = rc_run_deferred_storm_and_hash(SAND_CHUNK_PASS_SOLO);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(solo, duo,
                                        "two cores and the single-thread walk parted on the deferred "
                                        "queues");
    }
}
#endif

/* The reaction pass rides the same schedule, so a reacting board owes the
 * same answer as the sweep and the fluids: the queues and the shadows belong
 * to a lane, and a lane is a position's parity whoever executes it. */
static void
test_the_split_reaction_pass_ignores_how_its_lanes_interleave(void) {
    static const sand_chunk_pass_driver_t drivers[TC_DRIVERS] = {
        SAND_CHUNK_PASS_LANE0_EAGER, SAND_CHUNK_PASS_LANE1_EAGER, SAND_CHUNK_PASS_ALTERNATE};
    static const uint32_t seeds[] = {1u, 7u, 42u, 12345u, 99991u};

    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_SOLO);
        const uint32_t solo = rc_run_reaction_heavy_and_hash(TC_W, TC_H, seeds[i], 30, true);
        uint32_t driven[TC_DRIVERS];

        for (int d = 0; d < TC_DRIVERS; d++) {
            sand_chunk_pass_set_driver_for_test(drivers[d]);
            driven[d] = rc_run_reaction_heavy_and_hash(TC_W, TC_H, seeds[i], 30, true);
        }
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);

        for (int d = 0; d < TC_DRIVERS; d++) {
            char why[160];
            snprintf(why, sizeof why, "seed %u driver %d: a split reacting board depended on the lane interleaving",
                     (unsigned)seeds[i], (int)drivers[d]);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(solo, driven[d], why);
        }
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

/* The fluid passes alone, never through sand_step(): the gravity sweep
 * splits on its own terms and would drown out what this measures. */
static uint32_t
tc_run_fluids_and_hash(bool two_core, bool with_scratch) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    uint8_t* stamps = malloc(sand_step_stamp_bytes(TC_W, TC_H));
    void* scratch = malloc(sand_lane_scratch_bytes(TC_W, TC_H));
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(stamps);
    TEST_ASSERT_NOT_NULL(scratch);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 5u);
    sand_enable_sleeping(&s, blocks);
    sand_enable_step_stamps(&s, stamps);
    sand_enable_lane_scratch(&s, with_scratch ? scratch : NULL);
    sand_set_decay(&s, 0);

    for (int y = 1; y < TC_H - 1; y++) {
        for (int x = 1; x < TC_W - 1; x++) {
            if ((x * 7 + y * 13) % 5 == 0) {
                sand_set(&s, x, y, CELL_MAKE(MAT_WATER, 1 + (x + y) % 15));
            } else if ((x * 3 + y * 5) % 11 == 0) {
                sand_set(&s, x, y, GAS);
            }
        }
    }
    memset(blocks, BLOCK_HAS_LIQUID, (size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);

    static const int slide_a[] = {-1, 1};
    static const int slide_b[] = {1, 1};
    static const int perp_a[] = {1, 0};
    static const int perp_b[] = {-1, 0};
    const xflow_t flow = {.ax = {1, 0}, .dg = {1, 0}};

    sand_set_two_core_step(two_core);
    for (int step = 0; step < 12; step++) {
        s.step_phase = (uint16_t)step;
        sand_step_liquids(&s, &flow, 0, 1);
        sand_step_gas(&s, 0, 1000, 0, 1, slide_a, slide_b, perp_a, perp_b, 0, 1, 1, 0);
    }
    sand_set_two_core_step(false);

    uint32_t h = tc_hash(cells, (size_t)TC_W * (size_t)TC_H);
    h ^= tc_hash(blocks, (size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS) * 0x9E3779B1u;

    free(scratch);
    free(stamps);
    free(blocks);
    free(cells);
    return h;
}

static void
test_a_board_without_lane_scratch_steps_its_fluids_serially(void) {
    const uint32_t serial = tc_run_fluids_and_hash(false, false);
    const uint32_t unscratched = tc_run_fluids_and_hash(true, false);
    const uint32_t split = tc_run_fluids_and_hash(true, true);

    TEST_ASSERT_EQUAL_HEX32_MESSAGE(serial, unscratched,
                                    "two-core stepping without lane scratch must land on the serial board - "
                                    "there is nowhere for a lane's private bookkeeping to go");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(serial, split,
                                  "the same scene WITH lane scratch hashed the same as serial, so it never "
                                  "split and the comparison above proves nothing");
}

/* A lane writes its flags into a private copy, so the merge is the only
 * route back to the board. Every flag the board carries has to take it. */
static void
test_a_lane_merge_carries_every_content_flag_back(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 3u);
    sand_enable_sleeping(&s, blocks);
    void* scratch = lane_scratch_open(&s);
    clear_content_flags(&s);
    s.may_have_materials = 0;
    s.faller_may_move = false;
    blocks[0] = 0;

    sand_lane_t* const lanes = sand_lanes(&s);
    TEST_ASSERT_NOT_NULL(lanes);
    sand_lane_prepare(&lanes[0], &s);

    sand_t* const lane = &lanes[0].local;
    lane->may_have_liquid = true;
    lane->may_have_gas = true;
    lane->may_have_burning = true;
    lane->may_have_dissolver = true;
    lane->may_have_temperature = true;
    lane->may_have_moisture = true;
    lane->may_have_faller = true;
    lane->may_have_heat_holder = true;
    lane->may_have_condenser = true;
    lane->may_have_viscous_liquid = true;
    lane->may_have_materials = (uint16_t)(1u << MAT_WATER);
    lane->faller_may_move = true;
    lanes[0].blocks[0] |= (uint8_t)(BLOCK_HAS_LIQUID | BLOCK_HAS_MOISTURE);

    sand_lane_merge(&s, &lanes[0]);

    const sand_t merged = s;
    const uint8_t block = blocks[0];
    free(scratch);
    free(blocks);
    free(cells);

    TEST_ASSERT_TRUE_MESSAGE(merged.may_have_liquid, "may_have_liquid must survive the merge");
    TEST_ASSERT_TRUE_MESSAGE(merged.may_have_gas, "may_have_gas must survive the merge");
    TEST_ASSERT_TRUE_MESSAGE(merged.may_have_burning, "may_have_burning must survive the merge");
    TEST_ASSERT_TRUE_MESSAGE(merged.may_have_dissolver, "may_have_dissolver must survive the merge");
    TEST_ASSERT_TRUE_MESSAGE(merged.may_have_temperature, "may_have_temperature must survive the merge");
    TEST_ASSERT_TRUE_MESSAGE(merged.may_have_moisture, "may_have_moisture must survive the merge");
    TEST_ASSERT_TRUE_MESSAGE(merged.may_have_faller, "may_have_faller must survive the merge");
    TEST_ASSERT_TRUE_MESSAGE(merged.may_have_heat_holder, "may_have_heat_holder must survive the merge");
    TEST_ASSERT_TRUE_MESSAGE(merged.may_have_condenser, "may_have_condenser must survive the merge");
    TEST_ASSERT_TRUE_MESSAGE(merged.may_have_viscous_liquid, "may_have_viscous_liquid must survive the merge");
    TEST_ASSERT_TRUE_MESSAGE(merged.faller_may_move, "faller_may_move must survive the merge");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE((uint16_t)(1u << MAT_WATER), merged.may_have_materials,
                                    "may_have_materials must survive the merge");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(BLOCK_HAS_LIQUID | BLOCK_HAS_MOISTURE),
                                   (uint8_t)(block & (BLOCK_HAS_LIQUID | BLOCK_HAS_MOISTURE)),
                                   "a block's liquid and moisture bits must survive the merge");
}

#ifdef HOST_HEAP_ARENA
/* A frame has no budget for an allocation, and a core-1 half that misses
 * its join must not be writing into a block the caller has already given
 * back. Both are the same requirement: the passes allocate nothing. */
static void
test_a_split_fluid_step_allocates_nothing(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    uint8_t* stamps = malloc(sand_step_stamp_bytes(TC_W, TC_H));
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(stamps);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 11u);
    sand_enable_sleeping(&s, blocks);
    sand_enable_step_stamps(&s, stamps);
    void* scratch = lane_scratch_open(&s);
    sand_set_decay(&s, 0);
    for (int y = 1; y < TC_H - 1; y++) {
        for (int x = 1; x < TC_W - 1; x++) {
            if ((x + y) % 3 == 0) {
                sand_set(&s, x, y, CELL_MAKE(MAT_WATER, 1 + (x * 5 + y) % 15));
            } else if ((x + y) % 7 == 0) {
                sand_set(&s, x, y, GAS);
            }
        }
    }
    memset(blocks, BLOCK_HAS_LIQUID, (size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);

    static const int slide_a[] = {-1, 1};
    static const int slide_b[] = {1, 1};
    static const int perp_a[] = {1, 0};
    static const int perp_b[] = {-1, 0};
    const xflow_t flow = {.ax = {1, 0}, .dg = {1, 0}};

    size_t outstanding = 0;
    heap_arena_snapshot(NULL, &outstanding);
    heap_arena_reset_peak();

    sand_set_two_core_step(true);
    sand_step_liquids(&s, &flow, 0, 1);
    sand_step_gas(&s, 0, 1000, 0, 1, slide_a, slide_b, perp_a, perp_b, 0, 1, 1, 0);
    sand_set_two_core_step(false);

    const size_t peak = heap_arena_peak_bytes();

    free(scratch);
    free(stamps);
    free(blocks);
    free(cells);

    char why[160];
    snprintf(why, sizeof why, "a split liquid and gas step took the heap from %u to %u bytes", (unsigned)outstanding,
             (unsigned)peak);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)outstanding, (unsigned)peak, why);
}
#endif /* HOST_HEAP_ARENA */

/* Mostly gas, walled and lit, so the walk and the equalise pass both have
 * somewhere to go - the mixed scene's scattered grains never fill a shaft. */
static void
tc_build_gas_scene(sand_t* s, uint8_t* cells, uint32_t seed) {
    sand_init(s, cells, TC_W, TC_H, seed);

    rng_t r;
    rng_seed(&r, seed ^ 0x3C3C3C3Cu);

    for (int y = TC_H / 4; y < TC_H; y++) {
        for (int x = 0; x < TC_W; x++) {
            sand_set(s, x, y, (rng_below(&r, 16) == 0) ? STONE : GAS);
        }
    }
    sand_set(s, TC_W / 2, TC_H / 2, FIRE);
}

/* Chunks one step's sweep stepped. No block state, so nothing is settled and
 * the sweep visits every chunk of the plan - the count is the cut the pass
 * itself ran on, not what a helper says the cut would be. Sharing is pinned
 * on because the question here is which cut ran, not whether a cut this
 * coarse is worth two lanes; a plan the grid cannot take still returns 0. */
static unsigned
tc_chunks_swept_at(int side_x, int side_y) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t s;
    tc_build_scattered_scene(&s, cells, 5u);
    void* scratch = lane_scratch_open(&s);

    TEST_ASSERT_TRUE_MESSAGE(sand_chunk_side_for_test(side_x, side_y), "a side at or over the minimum must be taken");
    const unsigned before = sand_sweep_chunks_swept;
    const sand_chunk_share_t share = sand_chunk_share_for_test(SAND_CHUNK_SHARE_ALWAYS);
    sand_set_two_core_step(true);
    sand_step(&s, 0, 1000, 0);
    sand_set_two_core_step(false);
    (void)sand_chunk_share_for_test(share);
    tc_collect_core1();
    (void)sand_chunk_side_for_test(0, 0);

    free(scratch);
    free(cells);
    return sand_sweep_chunks_swept - before;
}

static void
tc_assert_cut_is_used(int side_x, int side_y) {
    const unsigned want = (unsigned)(((TC_W + side_x - 1) / side_x) * ((TC_H + side_y - 1) / side_y));
    const unsigned swept = tc_chunks_swept_at(side_x, side_y);
    char why[160];

    snprintf(why, sizeof why, "side %dx%d: the sweep stepped %u chunk(s), not the %u the cut has", side_x, side_y,
             swept, want);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(want, swept, why);
}

/* The cut each shipped grid takes, per travel class - sand.c's table said
 * back, so an edit to it has to be meant. The last two grids stay serial
 * under SAND_CHUNK_SPLIT_MIN_CELLS and only a forced side reaches their
 * rows. */
static void
test_each_quality_grid_takes_the_cut_its_travel_class_asks_for(void) {
    static const struct {
        int w, h;
        int along_x[2], other[2];
    } want[] = {
        {184, 224, {92, 17}, {47, 17}}, {122, 149, {61, 17}, {25, 17}}, {92, 112, {46, 17}, {23, 17}},
        {61, 74, {30, 17}, {17, 17}},   {46, 56, {23, 17}, {17, 17}},
    };

    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        for (int travel = 0; travel < SAND_CHUNK_TRAVEL_CLASSES; travel++) {
            const int* const expect = (travel == SAND_CHUNK_TRAVEL_X) ? want[i].along_x : want[i].other;
            sand_chunk_plan_t plan;
            int side_x, side_y;
            char why[160];

            sand_chunk_table_sides(want[i].w, want[i].h, (sand_chunk_travel_t)travel, &side_x, &side_y);
            snprintf(why, sizeof why, "%dx%d class %d: the table gave %dx%d, not %dx%d", want[i].w, want[i].h, travel,
                     side_x, side_y, expect[0], expect[1]);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expect[0], side_x, why);
            TEST_ASSERT_EQUAL_INT_MESSAGE(expect[1], side_y, why);
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(SAND_CHUNK_SIDE_MIN, side_x, why);
            TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(SAND_CHUNK_SIDE_MIN, side_y, why);
            snprintf(why, sizeof why, "%dx%d class %d: %dx%d is not a cut sand_chunk_plan() takes", want[i].w,
                     want[i].h, travel, side_x, side_y);
            TEST_ASSERT_TRUE_MESSAGE(sand_chunk_plan(&plan, want[i].w, want[i].h, side_x, side_y, 0, 0), why);
        }
    }
}

/* Only the sign of the y component decides, so a pass whose travel carries a
 * scan-order component lands in the second class however it moves. */
static void
test_only_a_step_travelling_along_x_alone_takes_the_long_cut(void) {
    TEST_ASSERT_EQUAL_INT(SAND_CHUNK_TRAVEL_X, sand_chunk_travel_of(1, 0));
    TEST_ASSERT_EQUAL_INT(SAND_CHUNK_TRAVEL_X, sand_chunk_travel_of(-1, 0));
    TEST_ASSERT_EQUAL_INT(SAND_CHUNK_TRAVEL_OTHER, sand_chunk_travel_of(0, 1));
    TEST_ASSERT_EQUAL_INT(SAND_CHUNK_TRAVEL_OTHER, sand_chunk_travel_of(0, -1));
    TEST_ASSERT_EQUAL_INT(SAND_CHUNK_TRAVEL_OTHER, sand_chunk_travel_of(1, 1));
    TEST_ASSERT_EQUAL_INT(SAND_CHUNK_TRAVEL_OTHER, sand_chunk_travel_of(-1, -1));
}

static void
tc_assert_forced_sides(const sand_t* probe, int force_x, int force_y, int want_x, int want_y) {
    for (int travel = 0; travel < SAND_CHUNK_TRAVEL_CLASSES; travel++) {
        int table_x, table_y, side_x, side_y;
        char why[160];

        sand_chunk_table_sides(probe->w, probe->h, (sand_chunk_travel_t)travel, &table_x, &table_y);
        TEST_ASSERT_TRUE(sand_chunk_side_for_test(force_x, force_y));
        sand_chunk_sides(probe, (sand_chunk_travel_t)travel, &side_x, &side_y);
        (void)sand_chunk_side_for_test(0, 0);

        snprintf(why, sizeof why, "class %d: forcing %dx%d gave %dx%d over the table's %dx%d", travel, force_x, force_y,
                 side_x, side_y, table_x, table_y);
        TEST_ASSERT_EQUAL_INT_MESSAGE(want_x != 0 ? want_x : table_x, side_x, why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(want_y != 0 ? want_y : table_y, side_y, why);
    }
}

/* An axis left at 0 keeps the table's own side, so a measurement can move
 * one axis without restating the other. */
static void
test_a_forced_chunk_side_beats_the_table_on_either_axis(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    sand_t* probe = malloc(sizeof *probe);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(probe);
    sand_init(probe, cells, TC_W, TC_H, 0u);

    tc_assert_forced_sides(probe, 33, 0, 33, 0);
    tc_assert_forced_sides(probe, 0, 29, 0, 29);
    tc_assert_forced_sides(probe, 33, 29, 33, 29);

    free(probe);
    free(cells);
}

/* A mark written under one pass's cut names another pass's chunk under a cut
 * of a different shape, so nothing may survive a pass boundary. */
static void
test_a_stamp_is_never_read_under_a_cut_it_was_not_written_under(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* stamps = malloc(sand_step_stamp_bytes(TC_W, TC_H));
    sand_t* s = malloc(sizeof *s);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(stamps);
    TEST_ASSERT_NOT_NULL(s);

    sand_init(s, cells, TC_W, TC_H, 0u);
    sand_enable_step_stamps(s, stamps);

    int wide_x, wide_y, narrow_x, narrow_y;
    sand_chunk_table_sides(TC_W, TC_H, SAND_CHUNK_TRAVEL_X, &wide_x, &wide_y);
    sand_chunk_table_sides(TC_W, TC_H, SAND_CHUNK_TRAVEL_OTHER, &narrow_x, &narrow_y);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(wide_x, narrow_x, "this board must cut its two classes differently to mean anything");

    sand_stamps_arm(s, wide_x, wide_y);
    sand_stamp_crossing(s, wide_x - 1, 0, wide_x, 0);
    TEST_ASSERT_TRUE_MESSAGE(sand_cell_stamped(s, wide_x, 0), "a crossing of the wide cut must mark where it landed");
    sand_stamp_crossing(s, narrow_x - 1, 0, narrow_x, 0);
    TEST_ASSERT_FALSE_MESSAGE(sand_cell_stamped(s, narrow_x, 0),
                              "a move inside one wide chunk is no crossing and must leave no mark");
    sand_stamps_disarm(s);

    sand_stamps_arm(s, narrow_x, narrow_y);
    TEST_ASSERT_FALSE_MESSAGE(sand_cell_stamped(s, wide_x, 0), "a mark written under one cut was honoured under "
                                                               "another");
    sand_stamp_crossing(s, narrow_x - 1, 0, narrow_x, 0);
    TEST_ASSERT_TRUE_MESSAGE(sand_cell_stamped(s, narrow_x, 0), "the same move is a crossing under the narrow cut");
    sand_stamps_disarm(s);

    free(s);
    free(stamps);
    free(cells);
}

/* A landscape step cuts its sweep one way and its gas spread another, so a
 * pass that armed the marks and never cleared them would hand the next cut a
 * mark naming a chunk it does not have. */
static void
test_a_step_hands_the_next_pass_no_marks_whatever_it_was_cut_by(void) {
    static const int gx[] = {1000, 0, 700};
    static const int gy[] = {0, 1000, 700};
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* stamps = malloc(sand_step_stamp_bytes(TC_W, TC_H));
    sand_t* s = malloc(sizeof *s);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(stamps);
    TEST_ASSERT_NOT_NULL(s);

    tc_build_scattered_scene(s, cells, 11u);
    sand_enable_step_stamps(s, stamps);
    void* scratch = lane_scratch_open(s);

    const unsigned swept_before = sand_split_dispatches[SAND_SPLIT_SLOT_SWEEP];
    const split_passes_scope_t split = split_passes_scope_begin(0u);
    sand_set_two_core_step(true);
    for (int i = 0; i < 12; i++) {
        const int arm = i % (int)(sizeof gx / sizeof gx[0]);
        char why[160];

        sand_step(s, gx[arm], gy[arm], 0);
        snprintf(why, sizeof why, "step %d under gravity %d,%d left the marks armed", i, gx[arm], gy[arm]);
        TEST_ASSERT_NULL_MESSAGE(s->stamps_live, why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, s->stamp_side_x, why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, s->stamp_side_y, why);
        for (size_t b = 0; b < sand_step_stamp_bytes(TC_W, TC_H); b++) {
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, stamps[b], why);
        }
    }
    sand_set_two_core_step(false);
    split_passes_scope_end(split);
    tc_collect_core1();
    tc_assert_split_arm_swept_split(swept_before, true);

    free(scratch);
    free(s);
    free(stamps);
    free(cells);
}

static void
test_a_chosen_chunk_side_is_the_cut_every_split_pass_runs(void) {
    TEST_ASSERT_FALSE_MESSAGE(sand_chunk_side_for_test(SAND_CHUNK_SIDE_MIN - 1, SAND_CHUNK_SIDE_MIN),
                              "a side under the minimum must be refused");
    TEST_ASSERT_FALSE_MESSAGE(sand_chunk_side_for_test(SAND_CHUNK_SIDE_MIN, SAND_CHUNK_SIDE_MIN - 1),
                              "a side under the minimum must be refused on either axis");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_chunk_side_forced[0], "a refused side must leave the override alone");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, sand_chunk_side_forced[1], "a refused side must leave the override alone");

    tc_assert_cut_is_used(32, 32);
    tc_assert_cut_is_used(46, 28);
    tc_assert_cut_is_used(23, 56);
    tc_assert_cut_is_used(TC_W / 2, TC_H / 2);
}

/* Both refusals sand_chunk_plan() makes, which the shipped rule can also
 * reach: one axis cut into a single chunk, and a cut past SAND_CHUNKS_MAX. */
static void
test_a_chunk_side_the_grid_cannot_take_falls_back_to_one_lane(void) {
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, tc_chunks_swept_at(TC_W, TC_H / 2), "one column of chunks must stay serial");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, tc_chunks_swept_at(TC_W / 2, TC_H), "one row of chunks must stay serial");
    TEST_ASSERT_GREATER_THAN_INT(SAND_CHUNKS_MAX, ((TC_W + SAND_CHUNK_SIDE_MIN - 1) / SAND_CHUNK_SIDE_MIN)
                                                      * ((TC_H + SAND_CHUNK_SIDE_MIN - 1) / SAND_CHUNK_SIDE_MIN));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, tc_chunks_swept_at(SAND_CHUNK_SIDE_MIN, SAND_CHUNK_SIDE_MIN),
                                   "a cut past SAND_CHUNKS_MAX must stay serial");
}

static void
tc_assert_no_sequential_draws(const char* scene) {
    char why[256];

    snprintf(why, sizeof why,
             "%s: a chunk-parallel pass took %u draw(s) from the sequential stream. "
             "A lane's board copy holds that stream and the join drops it, so both "
             "lanes replay the same numbers - use sand_rng_next_at() with a slot "
             "of its own at the site",
             scene, sand_split_sequential_draws);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, sand_split_sequential_draws, why);
}

/* Neither arm of a one-core-against-two comparison can see this: both run
 * lanes, so both replay the discarded stream identically. Only a count of the
 * draws themselves says whether a split pass has one. */
static void
test_no_split_pass_draws_from_the_sequential_stream(void) {
    static const struct {
        const char* name;
        void (*build)(sand_t*, uint8_t*, uint32_t);
    } scenes[] = {
        {"mixed", tc_build_scattered_scene},
        {"liquid", tc_build_liquid_scene},
        {"gas", tc_build_gas_scene},
    };

    for (size_t i = 0; i < sizeof scenes / sizeof scenes[0]; i++) {
        const unsigned swept_before = sand_sweep_chunks_swept;
        sand_split_sequential_draws = 0;
        (void)tc_run_scene_and_hash(scenes[i].build, 3u, 30, true);
        TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(swept_before, sand_sweep_chunks_swept,
                                              "the scene never took the split path, so zero proves nothing");
        tc_assert_no_sequential_draws(scenes[i].name);
    }

    sand_split_sequential_draws = 0;
    (void)rc_run_reaction_heavy_and_hash(TC_W, TC_H, 3u, 30, true);
    tc_assert_no_sequential_draws("reaction-heavy");
}

/* Cross-flow alone: no lane scratch, so the pass stays serial, and no gas,
 * whose busier draws would decide the hash on their own. The viscosity roll
 * is its one hashed draw, and a saturated pool under an uneven surface is
 * what gives cross-flow anything to roll for. */
static uint32_t
tc_run_crossflow_and_hash(void) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    sand_init(&s, cells, TC_W, TC_H, 5u);
    sand_enable_sleeping(&s, blocks);
    for (int y = TC_H / 3; y < TC_H; y++) {
        for (int x = 0; x < TC_W; x++) {
            sand_set(&s, x, y, CELL_MAKE(MAT_OIL, MASS_MAX));
        }
    }
    for (int x = 0; x < TC_W; x++) {
        sand_set(&s, x, TC_H / 3 - 1, CELL_MAKE(MAT_OIL, (uint8_t)(1 + (x * 7) % MASS_MAX)));
    }
    memset(blocks, BLOCK_HAS_LIQUID, (size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    /* Pinned strictly between 0 and 255, the two values that skip the roll,
     * and set by hand because sand_step() is what derives the flag. */
    sand_set_mobility(&s, 128);
    s.may_have_viscous_liquid = true;

    const xflow_t flow = {.ax = {1, 0}, .dg = {1, 0}};
    for (int step = 0; step < 12; step++) {
        s.step_phase = (uint16_t)step;
        sand_step_liquids(&s, &flow, 0, 1);
    }

    const uint32_t hash = tc_hash(cells, (size_t)TC_W * (size_t)TC_H);
    free(cells);
    free(blocks);
    return hash;
}

static void
tc_assert_override_moves_and_repeats(const char* what, uint32_t plain, uint32_t forced, uint32_t again) {
    char why[200];

    snprintf(why, sizeof why, "%s: forced hashed draws left the serial board where the plain stream left it", what);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(plain, forced, why);
    snprintf(why, sizeof why, "%s: two forced-hashed serial runs must land on the same board", what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(forced, again, why);
}

/* What the sweep's serial-hashed arm has to be worth measuring: the override
 * must reach every pass, not only the gravity sweep. The last two rows are
 * what say so - a whole step hashes its sweep either way, while the fluids
 * and reaction runners step their pass alone. */
static void
test_forced_hashed_draws_move_a_serial_board_and_repeat(void) {
    static const struct {
        const char* name;
        void (*build)(sand_t*, uint8_t*, uint32_t);
    } scenes[] = {
        {"mixed", tc_build_scattered_scene},
        {"liquid", tc_build_liquid_scene},
        {"gas", tc_build_gas_scene},
    };

    for (size_t i = 0; i < sizeof scenes / sizeof scenes[0]; i++) {
        const uint32_t plain = tc_run_scene_and_hash(scenes[i].build, 3u, 30, false);
        sand_force_hashed_rng(true);
        const uint32_t forced = tc_run_scene_and_hash(scenes[i].build, 3u, 30, false);
        const uint32_t again = tc_run_scene_and_hash(scenes[i].build, 3u, 30, false);
        sand_force_hashed_rng(false);
        tc_assert_override_moves_and_repeats(scenes[i].name, plain, forced, again);
    }

    const uint32_t fluids = tc_run_fluids_and_hash(false, true);
    sand_force_hashed_rng(true);
    const uint32_t fluids_forced = tc_run_fluids_and_hash(false, true);
    const uint32_t fluids_again = tc_run_fluids_and_hash(false, true);
    sand_force_hashed_rng(false);
    tc_assert_override_moves_and_repeats("fluids", fluids, fluids_forced, fluids_again);

    const uint32_t crossflow = tc_run_crossflow_and_hash();
    sand_force_hashed_rng(true);
    const uint32_t crossflow_forced = tc_run_crossflow_and_hash();
    const uint32_t crossflow_again = tc_run_crossflow_and_hash();
    sand_force_hashed_rng(false);
    tc_assert_override_moves_and_repeats("cross-flow", crossflow, crossflow_forced, crossflow_again);

    const uint32_t reacted = rc_run_reaction_heavy_and_hash(TC_W, TC_H, 3u, 30, false);
    sand_force_hashed_rng(true);
    const uint32_t reacted_forced = rc_run_reaction_heavy_and_hash(TC_W, TC_H, 3u, 30, false);
    const uint32_t reacted_again = rc_run_reaction_heavy_and_hash(TC_W, TC_H, 3u, 30, false);
    sand_force_hashed_rng(false);
    tc_assert_override_moves_and_repeats("reaction-heavy", reacted, reacted_forced, reacted_again);
}

/* The other half of the same claim, and the one the shipped board depends
 * on: armed and disarmed, the override leaves nothing behind. */
static void
test_the_hashed_draw_override_leaves_nothing_behind(void) {
    const uint32_t mixed = tc_run_scene_and_hash(tc_build_scattered_scene, 3u, 30, false);
    const uint32_t fluids = tc_run_fluids_and_hash(false, true);
    const uint32_t crossflow = tc_run_crossflow_and_hash();
    const uint32_t reacted = rc_run_reaction_heavy_and_hash(TC_W, TC_H, 3u, 30, false);

    sand_force_hashed_rng(true);
    (void)tc_run_scene_and_hash(tc_build_scattered_scene, 3u, 30, false);
    (void)tc_run_fluids_and_hash(false, true);
    (void)tc_run_crossflow_and_hash();
    (void)rc_run_reaction_heavy_and_hash(TC_W, TC_H, 3u, 30, false);
    sand_force_hashed_rng(false);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(mixed, tc_run_scene_and_hash(tc_build_scattered_scene, 3u, 30, false),
                                     "the mixed board changed after the override had been on and off again");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(fluids, tc_run_fluids_and_hash(false, true),
                                     "the fluid board changed after the override had been on and off again");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(crossflow, tc_run_crossflow_and_hash(),
                                     "the cross-flow board changed after the override had been on and off again");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(reacted, rc_run_reaction_heavy_and_hash(TC_W, TC_H, 3u, 30, false),
                                     "the reaction board changed after the override had been on and off again");
}

/* What one step of `build` dispatched and how much of the board was still
 * awake when it ended, so a caller can show it was asking the question it
 * meant to. */
typedef struct {
    unsigned total;
    unsigned per_pass[SAND_SPLIT_SLOTS];
    int awake_blocks;
} tc_dispatch_t;

/* Split passes one step of `build` dispatches, on a board armed the way the
 * app arms one. Blocks and marks both matter: without block state nothing is
 * ever settled, and without marks no pass is ready at all. */
static tc_dispatch_t
tc_dispatches_for(void (*build)(sand_t*, uint8_t*, uint32_t), uint32_t seed, int settle_steps, int gx, int gy) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    uint8_t* stamps = malloc(sand_step_stamp_bytes(TC_W, TC_H));
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(stamps);

    sand_t s;
    build(&s, cells, seed);
    sand_enable_sleeping(&s, blocks);
    sand_enable_step_stamps(&s, stamps);
    void* scratch = lane_scratch_open(&s);

    tc_dispatch_t out = {0};
    const two_core_scope_t core = two_core_scope_begin(true);
    for (int i = 0; i < settle_steps; i++) {
        sand_step(&s, gx, gy, 0);
    }
    memset(sand_split_dispatches, 0, sizeof sand_split_dispatches);
    sand_step(&s, gx, gy, 0);
    for (int slot = 0; slot < SAND_SPLIT_SLOTS; slot++) {
        out.per_pass[slot] = sand_split_dispatches[slot];
        out.total += sand_split_dispatches[slot];
    }
    out.awake_blocks = count_awake_blocks(&s);
    two_core_scope_end(core);
    tc_collect_core1();

    free(scratch);
    free(stamps);
    free(blocks);
    free(cells);
    return out;
}

/* A board that comes to rest and stays there: there is nothing for a lane to
 * take, so every pass pays prepare, merge, dispatch and join for a walk that
 * finds nothing. Stone rather than a sand pile - a poured pile of this size
 * still has grains trickling down its faces after eight hundred steps, so it
 * is not the case this is about. */
static void
tc_build_settled_slab_scene(sand_t* s, uint8_t* cells, uint32_t seed) {
    sand_init(s, cells, TC_W, TC_H, seed);
    for (int y = TC_H / 3; y < TC_H; y++) {
        for (int x = TC_W / 4; x < (TC_W * 3) / 4; x++) {
            sand_set(s, x, y, STONE);
        }
    }
}

/* One narrow column of sand falling down an otherwise empty board - awake
 * chunks in a single chunk column, which is a chain along travel whatever the
 * cut, so two lanes have nothing to overlap. */
static void
tc_build_falling_column_scene(sand_t* s, uint8_t* cells, uint32_t seed) {
    sand_init(s, cells, TC_W, TC_H, seed);
    for (int y = 0; y < TC_H / 8; y++) {
        for (int x = TC_W / 2 - 2; x < TC_W / 2 + 2; x++) {
            sand_set(s, x, y, SAND);
        }
    }
}

static void
test_a_settled_board_keeps_its_passes_on_one_core(void) {
    const tc_dispatch_t settled = tc_dispatches_for(tc_build_settled_slab_scene, 11u, 8, 0, 1000);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, settled.total, "a board with nothing awake must dispatch no split pass at all");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, settled.awake_blocks,
                                  "and the board must really have been asleep, or the test proves nothing");
}

static void
test_a_single_falling_column_keeps_its_passes_on_one_core(void) {
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0u, tc_dispatches_for(tc_build_falling_column_scene, 11u, 12, 0, 1000).total,
                                   "awake chunks in one line along travel are a chain, not two lanes");
}

static void
test_a_full_board_still_shares_its_passes(void) {
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0u, tc_dispatches_for(tc_build_scattered_scene, 3u, 2, 0, 1000).total,
                                          "a board busy everywhere must still reach the split path");
}

static void
test_a_busy_board_shares_its_sweep_whichever_way_it_travels(void) {
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(
        0u, tc_dispatches_for(tc_build_scattered_scene, 3u, 2, 0, 1000).per_pass[SAND_SPLIT_SLOT_SWEEP],
        "a board busy everywhere must share its sweep along y");
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(
        0u, tc_dispatches_for(tc_build_scattered_scene, 3u, 2, 1000, 0).per_pass[SAND_SPLIT_SLOT_SWEEP], "and along x");
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(
        0u, tc_dispatches_for(tc_build_dry_scattered_scene, 3u, 2, 0, 1000).per_pass[SAND_SPLIT_SLOT_SWEEP],
        "and so must one with no liquid on it");
}

static void
tc_assert_same_dispatch(tc_dispatch_t want, tc_dispatch_t got, const char* why) {
    for (int slot = 0; slot < SAND_SPLIT_SLOTS; slot++) {
        TEST_ASSERT_EQUAL_UINT_MESSAGE(want.per_pass[slot], got.per_pass[slot], why);
    }
}

/* The decision reads board state only, so it cannot depend on which thread
 * ran the lanes last time or on how many times it has been asked - pass by
 * pass, since a sweep that flipped and a gas walk that flipped back would
 * leave a total unmoved. */
static void
test_the_sharing_decision_is_the_same_whoever_drove_the_lanes(void) {
    static const sand_chunk_pass_driver_t drivers[] = {SAND_CHUNK_PASS_CORE1, SAND_CHUNK_PASS_SOLO,
                                                       SAND_CHUNK_PASS_ALTERNATE, SAND_CHUNK_PASS_LANE1_EAGER};
    const tc_dispatch_t liquid = tc_dispatches_for(tc_build_scattered_scene, 3u, 2, 0, 1000);
    const tc_dispatch_t dry = tc_dispatches_for(tc_build_dry_scattered_scene, 3u, 2, 0, 1000);
    const tc_dispatch_t settled = tc_dispatches_for(tc_build_settled_slab_scene, 11u, 8, 0, 1000);

    for (size_t i = 0; i < sizeof drivers / sizeof drivers[0]; i++) {
        char why[160];

        sand_chunk_pass_set_driver_for_test(drivers[i]);
        for (int again = 0; again < 2; again++) {
            snprintf(why, sizeof why, "driver %d, run %d shared a different set of split passes", (int)i, again);
            tc_assert_same_dispatch(liquid, tc_dispatches_for(tc_build_scattered_scene, 3u, 2, 0, 1000), why);
            tc_assert_same_dispatch(dry, tc_dispatches_for(tc_build_dry_scattered_scene, 3u, 2, 0, 1000), why);
            tc_assert_same_dispatch(settled, tc_dispatches_for(tc_build_settled_slab_scene, 11u, 8, 0, 1000), why);
        }
    }
    sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);
}

void
run_sand_two_core_suite(void) {
    RUN_TEST(test_two_core_step_is_deterministic_across_seeds);
    RUN_TEST(test_the_split_sweep_ignores_how_its_lanes_interleave);
    RUN_TEST(test_the_split_liquid_pass_ignores_how_its_lanes_interleave);
    RUN_TEST(test_split_gas_walk_uses_hashed_rng);
    RUN_TEST(test_split_gas_spread_never_hops_into_a_chunk_ranked_later);
    RUN_TEST(test_the_split_gas_passes_ignore_how_their_lanes_interleave);
    RUN_TEST(test_two_core_step_actually_changes_the_draw_stream);
    RUN_TEST(test_two_core_step_changes_the_draw_stream_at_the_smallest_split_quality);
    RUN_TEST(test_a_grid_under_the_split_floor_steps_serially_unless_a_side_is_forced);
    RUN_TEST(test_two_core_step_does_not_leak_or_fabricate_mass);
    RUN_TEST(test_a_settled_pile_under_two_core_stepping_shows_no_tile_seam);
    RUN_TEST(test_two_core_step_never_double_moves_at_a_seam);
    RUN_TEST(test_a_liquid_crossing_a_chunk_border_stays_in_cross_flow_reach);
    RUN_TEST(test_two_core_step_matches_serial_fall_distance_at_a_seam);
    RUN_TEST(test_smaller_quality_seams_match_serial);
    RUN_TEST(test_a_serial_falling_body_of_sand_opens_no_gap);
    RUN_TEST(test_a_split_falling_body_of_sand_opens_no_gap);
    RUN_TEST(test_a_serial_falling_body_of_water_opens_no_gap);
    RUN_TEST(test_a_split_rising_body_of_gas_opens_no_gap);
    RUN_TEST(test_a_split_falling_body_of_water_opens_no_gap);
    RUN_TEST(test_a_fuse_blast_throws_grains_on_both_cores);
    RUN_TEST(test_a_lava_burst_throws_grains_on_both_cores);
    RUN_TEST(test_a_settled_chunk_does_no_row_work);
    RUN_TEST(test_two_core_step_conserves_grains_on_a_dense_column_and_pile);
    RUN_TEST(test_reaction_split_matches_serial_on_a_zero_randomness_fire_chain);
    RUN_TEST(test_reaction_split_is_deterministic_across_seeds);
    RUN_TEST(test_every_deferred_reaction_effect_is_applied_exactly_once);
    RUN_TEST(test_the_split_reaction_pass_ignores_how_its_lanes_interleave);
    RUN_TEST(test_reaction_split_actually_changes_the_draw_stream);
    RUN_TEST(test_landscape_water_column_has_no_line_mass_lag);
    RUN_TEST(test_split_gas_equalise_keeps_seam_order);
    RUN_TEST(test_split_gas_equalise_hops_once_across_a_chunk_column);
    RUN_TEST(test_no_split_pass_draws_from_the_sequential_stream);
    RUN_TEST(test_forced_hashed_draws_move_a_serial_board_and_repeat);
    RUN_TEST(test_the_hashed_draw_override_leaves_nothing_behind);
    RUN_TEST(test_each_quality_grid_takes_the_cut_its_travel_class_asks_for);
    RUN_TEST(test_only_a_step_travelling_along_x_alone_takes_the_long_cut);
    RUN_TEST(test_a_forced_chunk_side_beats_the_table_on_either_axis);
    RUN_TEST(test_a_stamp_is_never_read_under_a_cut_it_was_not_written_under);
    RUN_TEST(test_a_step_hands_the_next_pass_no_marks_whatever_it_was_cut_by);
    RUN_TEST(test_a_chosen_chunk_side_is_the_cut_every_split_pass_runs);
    RUN_TEST(test_a_chunk_side_the_grid_cannot_take_falls_back_to_one_lane);
    RUN_TEST(test_a_board_without_lane_scratch_steps_its_fluids_serially);
    RUN_TEST(test_a_lane_merge_carries_every_content_flag_back);
    RUN_TEST(test_a_settled_board_keeps_its_passes_on_one_core);
    RUN_TEST(test_a_single_falling_column_keeps_its_passes_on_one_core);
    RUN_TEST(test_a_full_board_still_shares_its_passes);
    RUN_TEST(test_a_busy_board_shares_its_sweep_whichever_way_it_travels);
    RUN_TEST(test_the_sharing_decision_is_the_same_whoever_drove_the_lanes);
#ifdef HOST_HEAP_ARENA
    RUN_TEST(test_a_split_fluid_step_allocates_nothing);
#endif
#ifdef DEVICE_BUILD
    RUN_TEST(test_a_core_1_lane_lands_on_the_solo_board);
    RUN_TEST(test_a_core_1_reaction_lane_lands_on_the_solo_heavy_board);
    RUN_TEST(test_a_core_1_reaction_lane_lands_on_the_solo_board);
    RUN_TEST(test_a_timed_out_job_falls_back_inline);
#endif
}

SUITE_REGISTER(run_sand_two_core_suite);

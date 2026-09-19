/* Portable checks for the chunk-parallel passes - the gravity sweep on its
 * schedule, cross-flow, gas and reactions in four colours: deterministic,
 * mass-conserving, and without a chunk seam. Host jobs run inline. */
#include <stdio.h>
#include <stdlib.h>

#include "sand.h"
#include "sand_priv.h"
#include "suite_sand_common.h"
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

_Static_assert(TC_H >= SAND_CHUNK_SIDE_MIN * SAND_CHUNK_SPLIT_MIN_ROWS,
               "the two-core suite needs enough chunk rows to split");

/* Every chunk claim below has to hold across the app's whole quality range,
 * not at one size: the board it ships and the smallest one it offers. */
static void
tc_for_each_quality(void (*check)(const sand_t* s)) {
    static const int qualities[][2] = {
        {REAL_W, REAL_H}, {REAL_W / SAND_CHUNK_TARGET_CELLS_DIVISOR, REAL_H / SAND_CHUNK_TARGET_CELLS_DIVISOR}};

    for (size_t q = 0; q < sizeof qualities / sizeof qualities[0]; q++) {
        uint8_t* cells = malloc((size_t)qualities[q][0] * (size_t)qualities[q][1]);
        TEST_ASSERT_NOT_NULL(cells);

        sand_t sand;
        sand_init(&sand, cells, qualities[q][0], qualities[q][1], (uint32_t)q);
        check(&sand);
        free(cells);
    }
}

static const int tc_ring[][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};

static void
tc_assert_no_neighbour_shares_colour(const sand_t* s, int cx, int cy) {
    for (size_t i = 0; i < sizeof tc_ring / sizeof tc_ring[0]; i++) {
        const int nx = cx + tc_ring[i][0];
        const int ny = cy + tc_ring[i][1];
        if ((unsigned)nx >= (unsigned)sand_chunk_cols(s) || (unsigned)ny >= (unsigned)sand_chunk_rows(s)) {
            continue;
        }
        TEST_ASSERT_NOT_EQUAL_INT(sand_chunk_color(cx, cy), sand_chunk_color(nx, ny));
    }
}

static void
tc_check_neighbour_colours_differ(const sand_t* s) {
    TEST_ASSERT_GREATER_OR_EQUAL_INT(SAND_CHUNK_SIDE_MIN, sand_chunk_side(s));
    for (int cy = 0; cy < sand_chunk_rows(s); cy++) {
        for (int cx = 0; cx < sand_chunk_cols(s); cx++) {
            tc_assert_no_neighbour_shares_colour(s, cx, cy);
        }
    }
}

static void
test_chunk_colours_separate_every_touching_chunk(void) {
    tc_for_each_quality(tc_check_neighbour_colours_differ);
}

static void
tc_count_chunk_cells(const sand_t* s, uint8_t* seen, int cx, int cy) {
    int x0, x1, y0, y1;
    sand_chunk_span(cx, sand_chunk_side(s), s->w, &x0, &x1);
    sand_chunk_span(cy, sand_chunk_side(s), s->h, &y0, &y1);

    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            seen[y * s->w + x]++;
        }
    }
}

/* Four colours are a partition only if between them they cover the board
 * once: a cell no colour claims is never stepped, and one two colours claim
 * is stepped twice. */
static void
tc_check_colours_cover_every_cell_once(const sand_t* s) {
    uint8_t* seen = calloc((size_t)s->w * (size_t)s->h, 1);
    TEST_ASSERT_NOT_NULL(seen);

    for (int color = 0; color < SAND_CHUNK_COLOR_COUNT; color++) {
        for (int cy = 0; cy < sand_chunk_rows(s); cy++) {
            for (int cx = 0; cx < sand_chunk_cols(s); cx++) {
                if (sand_chunk_color(cx, cy) == color) {
                    tc_count_chunk_cells(s, seen, cx, cy);
                }
            }
        }
    }

    for (int i = 0; i < s->w * s->h; i++) {
        TEST_ASSERT_EQUAL_UINT8(1, seen[i]);
    }
    free(seen);
}

static void
test_chunk_colours_cover_every_cell_exactly_once(void) {
    tc_for_each_quality(tc_check_colours_cover_every_cell_once);
}

static void
tc_assert_chunk_rows_clear_each_other(const sand_t* s, int a, int b) {
    int a0, a1, b0, b1;
    sand_chunk_span(a, sand_chunk_side(s), s->h, &a0, &a1);
    sand_chunk_span(b, sand_chunk_side(s), s->h, &b0, &b1);

    const int gap = (a0 > b0) ? a0 - b1 : b0 - a1;
    TEST_ASSERT_GREATER_THAN_INT(SAND_LIQUID_SIGHT, gap);
}

/* The two claims sand_chunk_share() makes for the chunk rows of one colour:
 * both workers get some, and no two the workers hold at once come within the
 * furthest a split pass writes from the cell it is stepping. The second is
 * what lets a worker write the board's own row-indexed bookkeeping rather
 * than a private copy. */
static void
tc_check_one_colours_workers(const sand_t* s, int row_parity) {
    int owned[2] = {0, 0};

    for (int a = row_parity; a < sand_chunk_rows(s); a += 2) {
        owned[sand_chunk_share(a)]++;
        for (int b = row_parity; b < sand_chunk_rows(s); b += 2) {
            if (sand_chunk_share(a) != sand_chunk_share(b)) {
                tc_assert_chunk_rows_clear_each_other(s, a, b);
            }
        }
    }

    if (sand_chunk_split_ready(s)) {
        TEST_ASSERT_GREATER_THAN_INT(0, owned[0]);
        TEST_ASSERT_GREATER_THAN_INT(0, owned[1]);
    }
}

static void
tc_check_workers_never_meet_on_a_row(const sand_t* s) {
    for (int row_parity = 0; row_parity < 2; row_parity++) {
        tc_check_one_colours_workers(s, row_parity);
    }
}

static void
test_a_colours_two_workers_never_meet_on_a_row(void) {
    tc_for_each_quality(tc_check_workers_never_meet_on_a_row);
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

/* A mixed, seed-varied scatter: powders, two liquids of different
 * density, a gas and a static, at a random third of the grid each - so
 * every chunk, on both sides of every boundary, has something to move,
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
 * not just straight down - so a chunk boundary is crossed both ways and in
 * both axes. */
static uint32_t
tc_run_and_hash(uint32_t seed, int steps, bool two_core) {
    uint8_t* cells = malloc((size_t)TC_W * (size_t)TC_H);
    uint8_t* blocks = malloc((size_t)TC_BLOCK_COLS * (size_t)TC_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t s;
    tc_build_scattered_scene(&s, cells, seed);
    sand_enable_sleeping(&s, blocks);
    void* scratch = lane_scratch_open(&s);

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

    free(scratch);
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
    sand_set_two_core_step(two_core);
    for (int i = 0; i < 40; i++) {
        const int arm = i % (int)(sizeof gx / sizeof gx[0]);
        sand_step(&s, gx[arm], gy[arm], 0);
    }
    sand_set_two_core_step(false);

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

#define TC_DRIVERS 3

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
        const uint32_t solo = tc_run_and_hash(seeds[i], 40, true);
        uint32_t driven[TC_DRIVERS];

        for (int d = 0; d < TC_DRIVERS; d++) {
            sand_chunk_pass_set_driver_for_test(drivers[d]);
            driven[d] = tc_run_and_hash(seeds[i], 40, true);
        }
        sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_SOLO);

        for (int d = 0; d < TC_DRIVERS; d++) {
            char why[160];
            snprintf(why, sizeof why, "seed %u driver %d: the split sweep's board depended on the lane interleaving",
                     (unsigned)seeds[i], (int)drivers[d]);
            TEST_ASSERT_EQUAL_HEX32_MESSAGE(solo, driven[d], why);
        }
    }
}

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
tc_run_gas_walk_and_hash(uint32_t seed, bool reverse_workers) {
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
    free(scratch);
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
                                        "changing which worker owns each chunk row must not change the gas walk");
    }
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
    void* scratch = lane_scratch_open(&s);

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
    worst_row_deltas(occupied, TC_H, sand_chunk_side(&s), &interior_worst, &boundary_worst);

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

/* A sand_t is too large a frame for a test that also holds fixtures - see
 * this suite's own stack ceiling - so the probe this needs lives on the
 * heap. */
static int
tc_chunk_side_of(int w, int h) {
    uint8_t* cells = malloc((size_t)w * (size_t)h);
    sand_t* probe = malloc(sizeof *probe);
    TEST_ASSERT_NOT_NULL(cells);
    TEST_ASSERT_NOT_NULL(probe);

    sand_init(probe, cells, w, h, 0u);
    const int side = sand_chunk_side(probe);

    free(probe);
    free(cells);
    return side;
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

static void
tc_board_step(tc_board_t* b, int gx, int gy, bool two_core) {
    sand_set_two_core_step(two_core);
    sand_step(&b->s, gx, gy, 0);
    sand_set_two_core_step(false);
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
    const int side = tc_chunk_side_of(TC_W, TC_H);

    for (int phase = 0; phase < 2; phase++) {
        tc_assert_row_seams_move_one_cell(phase, side);
        tc_assert_corners_move_one_cell(phase, side);
    }
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

static void
test_a_liquid_crossing_a_chunk_border_stays_in_cross_flow_reach(void) {
    const int side = tc_chunk_side_of(TC_W, TC_H);
    const int x = (TC_W / 2 / SAND_BLOCK_W) * SAND_BLOCK_W + SAND_BLOCK_W / 2;
    const int y = (side / 2 / SAND_BLOCK_H) * SAND_BLOCK_H + SAND_BLOCK_H / 2;

    tc_assert_a_crossing_liquid_marks_where_it_lands(0, 1000, x, side - 1, x, side);
    tc_assert_a_crossing_liquid_marks_where_it_lands(1000, 0, side - 1, y, side, y);
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
        const int side = tc_chunk_side_of(qualities[q].w, qualities[q].h);
        for (int phase = 0; phase < 2; phase++) {
            tc_assert_quality_row_seams_match_serial(qualities[q].w, qualities[q].h, side, phase);
            tc_assert_quality_corners_match_serial(qualities[q].w, qualities[q].h, side, phase);
        }
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
    const int side = tc_chunk_side_of(TC_W, TC_H);
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
    void* scratch = lane_scratch_open(&split);
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

    free(scratch);
    free(serial_cells);
    free(split_cells);
    free(serial_blocks);
    free(split_blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, worst_row_mass,
                                  "the split landscape water column left liquid mass in a different row than serial");
}

static int
tc_first_gas_equalise_boundary(const sand_t* s) {
    return sand_chunk_side(s);
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
    for (int phase = 0; phase < SAND_CHUNK_COLOR_COUNT; phase++) {
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
    const int side = tc_chunk_side_of(TC_W, TC_H);

    for (int boundary = side; boundary < TC_W; boundary += side) {
        for (int x = boundary - 1; x <= boundary; x++) {
            tc_assert_gas_hop_matches_serial(x, side / 2, false);
            tc_assert_gas_hop_matches_serial(x, side / 2, true);
        }
    }
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

void
run_sand_two_core_suite(void) {
    RUN_TEST(test_chunk_colours_separate_every_touching_chunk);
    RUN_TEST(test_chunk_colours_cover_every_cell_exactly_once);
    RUN_TEST(test_a_colours_two_workers_never_meet_on_a_row);
    RUN_TEST(test_two_core_step_is_deterministic_across_seeds);
    RUN_TEST(test_the_split_sweep_ignores_how_its_lanes_interleave);
    RUN_TEST(test_split_gas_walk_uses_hashed_rng);
    RUN_TEST(test_split_gas_walk_ignores_worker_order);
    RUN_TEST(test_two_core_step_actually_changes_the_draw_stream);
    RUN_TEST(test_two_core_step_changes_the_draw_stream_at_smaller_qualities);
    RUN_TEST(test_two_core_step_does_not_leak_or_fabricate_mass);
    RUN_TEST(test_a_settled_pile_under_two_core_stepping_shows_no_tile_seam);
    RUN_TEST(test_two_core_step_never_double_moves_at_a_seam);
    RUN_TEST(test_a_liquid_crossing_a_chunk_border_stays_in_cross_flow_reach);
    RUN_TEST(test_two_core_step_matches_serial_fall_distance_at_a_seam);
    RUN_TEST(test_smaller_quality_seams_match_serial);
    RUN_TEST(test_a_serial_falling_body_of_sand_opens_no_gap);
    RUN_TEST(test_a_split_falling_body_of_sand_opens_no_gap);
    RUN_TEST(test_a_serial_falling_body_of_water_opens_no_gap);
    RUN_TEST(test_a_split_falling_body_of_water_opens_no_gap);
    RUN_TEST(test_a_fuse_blast_throws_grains_on_both_cores);
    RUN_TEST(test_a_lava_burst_throws_grains_on_both_cores);
    RUN_TEST(test_a_settled_chunk_does_no_row_work);
    RUN_TEST(test_two_core_step_conserves_grains_on_a_dense_column_and_pile);
    RUN_TEST(test_reaction_split_matches_serial_on_a_zero_randomness_fire_chain);
    RUN_TEST(test_reaction_split_is_deterministic_across_seeds);
    RUN_TEST(test_reaction_split_actually_changes_the_draw_stream);
    RUN_TEST(test_landscape_water_column_has_no_row_mass_lag);
    RUN_TEST(test_split_gas_equalise_keeps_seam_order);
    RUN_TEST(test_split_gas_equalise_hops_once_across_a_chunk_column);
    RUN_TEST(test_a_board_without_lane_scratch_steps_its_fluids_serially);
    RUN_TEST(test_a_lane_merge_carries_every_content_flag_back);
#ifdef HOST_HEAP_ARENA
    RUN_TEST(test_a_split_fluid_step_allocates_nothing);
#endif
#ifdef DEVICE_BUILD
    RUN_TEST(test_a_timed_out_job_falls_back_inline);
#endif
}

SUITE_REGISTER(run_sand_two_core_suite);

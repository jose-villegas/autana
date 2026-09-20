/*
 * Portable suite: the falling-sand automaton - frame-budget performance
 * against the shared benchmark scenes, plus the app-level allocation
 * selfcheck and a couple of full-grid acid-bubble tests.
 *
 * DEVICE_BUILD-only, almost entirely: wall-clock frame-budget assertions
 * are meaningless on a host whose CPU speed bears no relation to the
 * device's, so nearly every test below only compiles into the on-device
 * selftest image, not the host runner - see each test's own #ifdef
 * DEVICE_BUILD guard and RUN_TEST line.
 *
 * Split out of suite_sand.c, which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h}; the scene builders these frame-budget
 * tests measure live in suite_sand_scenes.{c,h} - see those headers.
 */
#include <inttypes.h>
#include <math.h> /* not every file in the split still needs atan2()/M_PI,
                     * but every file inherited suite_sand.c's own include
                     * block rather than being pruned by hand, to keep the
                     * split itself mechanical and low-risk */
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
/* Not every libc defines this in <math.h> without a feature-test macro this
 * file has no other reason to set (MinGW's, notably, on the host build) -
 * cheaper to supply it directly than to widen this file's own feature-test
 * exposure for one constant. */
#define M_PI 3.14159265358979323846
#endif

#include "suites.h"
#include "unity.h"

#include "material_palette.h"
#include "sand.h"
#include "sand_priv.h"
#include "suite_sand_common.h"
#include "suite_sand_scenes.h"
#include "tilt.h" /* TILT_TAU_*_MS - the turn below follows the real
                       * filter shape rather than a straight line */
#include "util/intmath.h"

#define REAL_BLOCK_COLS ((REAL_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define REAL_BLOCK_ROWS ((REAL_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)

/* Sand and dirt in equal amounts under water would soak; this instead pairs
 * sand against water across a settled stone-X divider that never lets the
 * two touch, so the reaction pass stays alive only on the wettable term
 * (MAT_SAND's own soaks!=0) - the case sand_step_reactions()'s soak-only
 * skip exists for. Portable, not DEVICE_BUILD-only: the host regression
 * suite for that skip (suite_sand_dirt.c) reruns this same scene, and must
 * see exactly what the frame-budget test below measures. */
static void
build_mixed_gravity_flip_scene(sand_t* real, uint8_t* big, uint8_t* blocks) {
    sand_init(real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(real, blocks);

    const int sand_x1 = (REAL_W * 3) / 10;           /* ~30% from the left */
    const int water_x0 = REAL_W - (REAL_W * 3) / 10; /* ~30% from the right */

    for (int y = REAL_H / 2; y < REAL_H; y++) {
        for (int x = 0; x < sand_x1; x++) {
            sand_set(real, x, y, SAND_FIRST_SHADE);
        }
        for (int x = water_x0; x < REAL_W; x++) {
            sand_set(real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }

    const int mid_w = water_x0 - sand_x1;
    for (int y = 0; y < REAL_H; y++) {
        const int off = (y * (mid_w - 1)) / (REAL_H - 1);
        const int xa = sand_x1 + off;
        const int xb = water_x0 - 1 - off;
        const int xa2 = (xa + 1 < water_x0) ? xa + 1 : xa;
        const int xb2 = (xb - 1 >= sand_x1) ? xb - 1 : xb;
        sand_set(real, xa, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(real, xa2, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(real, xb, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        sand_set(real, xb2, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
    }

    /* Let it fully settle first - same starting state a real pour-then-
     * pause reaches, stone included (it was never moving, but the pass
     * still has to notice that). */
    for (int i = 0; i < 300; i++) {
        sand_step(real, 0, 1000, 0);
    }
}

/* FNV-1a over the grid, so a host build of the same scene can be compared
 * byte-for-byte against the device at the same steps. Portable for the same
 * reason build_mixed_gravity_flip_scene() is. */
static uint32_t
grid_hash(const uint8_t* grid, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= grid[i];
        h *= 16777619u;
    }
    return h;
}

/* Cells a soak-only pass would visit for THIS board right now: summed
 * BLOCK_LIQUID_NEAR block areas, clipped to the grid edge exactly the way
 * step_one_reacting_row_liquid_near() (sand_reactions.c) clips them. The
 * real bound the fast path is built from, not an estimate. */
static long
liquid_near_cell_bound(const sand_t* s) {
    long total = 0;
    for (int by = 0; by < s->block_rows; by++) {
        const int y_lo = by * SAND_BLOCK_H;
        const int y_hi = (y_lo + SAND_BLOCK_H < s->h) ? y_lo + SAND_BLOCK_H : s->h;
        for (int bx = 0; bx < s->block_cols; bx++) {
            if ((s->block_state[(size_t)by * (size_t)s->block_cols + (size_t)bx] & BLOCK_LIQUID_NEAR) == 0) {
                continue;
            }
            const int x_lo = bx * SAND_BLOCK_W;
            const int x_hi = (x_lo + SAND_BLOCK_W < s->w) ? x_lo + SAND_BLOCK_W : s->w;
            total += (long)(x_hi - x_lo) * (long)(y_hi - y_lo);
        }
    }
    return total;
}

/* THE REGRESSION: MAT_SAND soaks, so this scene - a stone
 * wall keeps its sand and water apart - still walked its full grid every
 * step. sand_step_reactions()'s soak-only skip walks only BLOCK_LIQUID_NEAR
 * blocks instead - see its own soak_only comment.
 *
 * Runs the SAME scene and steps twice so "far fewer" reads against a
 * measured full-walk count, not a guess; the LIQUID_NEAR bound is likewise
 * summed fresh per step, since flipping gravity moves the marked blocks. */
static void
test_the_soak_only_skip_dispatches_far_fewer_cells_than_a_full_walk(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    const int steps = 20;
    sand_t real;

    build_mixed_gravity_flip_scene(&real, big, blocks);
    sand_reactions_force_full_walk(true);
    sand_reactions_cells_dispatched = 0;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const unsigned dispatched_full = sand_reactions_cells_dispatched;

    build_mixed_gravity_flip_scene(&real, big, blocks);
    sand_reactions_force_full_walk(false);
    sand_reactions_cells_dispatched = 0;
    long near_bound = 0;
    for (int i = 0; i < steps; i++) {
        /* Sampled AFTER the step, not before: the liquid pass inside
         * sand_step() refreshes BLOCK_LIQUID_NEAR before reactions runs, so
         * the state reactions actually saw this step is the state left
         * behind at the end of it, not the one entering it. */
        sand_step(&real, 0, -1000, 0);
        near_bound += liquid_near_cell_bound(&real);
    }
    const unsigned dispatched_fast = sand_reactions_cells_dispatched;

    sand_reactions_force_full_walk(false); /* restore the shipped default */
    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)(REAL_W * REAL_H * steps), dispatched_full,
                                   "sanity: forcing the full walk must dispatch every cell of every step");

    char why[220];
    snprintf(why, sizeof why,
             "the soak-only skip must dispatch exactly the BLOCK_LIQUID_NEAR bound, not the full grid - "
             "bound=%ld fast=%u full=%u",
             near_bound, dispatched_fast, dispatched_full);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)near_bound, dispatched_fast, why);
    TEST_ASSERT_LESS_THAN_MESSAGE(dispatched_full / 2, dispatched_fast, why);
}

/* THE FINGERPRINT: this exact scene and step count is also
 * report_fingerprint.sh's device/host equivalence anchor - see its own
 * top comment for why 20 flip steps and this hash. Kept here beside the
 * scene it hashes rather than duplicated. */
#define MIXED_FLIP_20_STEP_HASH 0x6a6aa1cfu

/* Equivalence half of the regression above: the soak-only skip must be
 * byte-identical to the reference full walk. Pinned to serial like
 * report_fingerprint.sh's own capture (grid_fingerprint.c) - two-core
 * stepping is deliberately a different hash (Sand-Simulation.md). */
static void
test_the_soak_only_skip_matches_the_full_walks_grid_exactly(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    const bool two_core_before = sand_two_core_step_enabled();
    sand_set_two_core_step(false);

    sand_t real;
    build_mixed_gravity_flip_scene(&real, big, blocks);

    sand_reactions_force_full_walk(true);
    for (int i = 0; i < 20; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const uint32_t full_hash = grid_hash(big, (size_t)REAL_W * (size_t)REAL_H);

    build_mixed_gravity_flip_scene(&real, big, blocks);
    sand_reactions_force_full_walk(false);
    for (int i = 0; i < 20; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const uint32_t fast_hash = grid_hash(big, (size_t)REAL_W * (size_t)REAL_H);

    sand_reactions_force_full_walk(false);
    sand_set_two_core_step(two_core_before);
    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_HEX32_MESSAGE(full_hash, fast_hash,
                                    "the soak-only skip must reproduce the full walk's grid exactly");
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(MIXED_FLIP_20_STEP_HASH, fast_hash,
                                    "the mixed flip scene's hash must stay pegged - a change here without "
                                    "an accompanying report_fingerprint.sh --update means behaviour moved, "
                                    "not just performance");
}

/* A suite run alone (RUNSUITE, or this suite's own perf scope) inherits
 * two_core_step_on's raw ESP_PLATFORM default - true - since nothing ahead
 * of it in that run has pinned it false yet. The equivalence test above
 * must not care either way. */
static void
test_the_soak_only_skip_hash_survives_ambient_two_core_state(void) {
    const bool two_core_before = sand_two_core_step_enabled();

    sand_set_two_core_step(true);
    test_the_soak_only_skip_matches_the_full_walks_grid_exactly();

    sand_set_two_core_step(false);
    test_the_soak_only_skip_matches_the_full_walks_grid_exactly();

    sand_set_two_core_step(two_core_before);
}

#ifdef DEVICE_BUILD
#include <stdlib.h>
#include "../../gfx/gfx.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "row_runs.h"
#include "xtensa/xt_perf_consts.h"
#include "xtensa_perfmon_access.h"

static void log_pass_split(const char* name, int steps, int impulse_max, const int64_t totals[6], const int64_t peak[6],
                           int peak_impulses, unsigned cap_hits);

static int perf_unmet_targets;
static bool gas_ab_reporting;

/* Ceilings are worst + max(spread, 2% of worst) across 5 two-core-pinned S3
 * portrait captures, 2026-09-16, build a195574e7177-dirty-diag. Landscape
 * costs 17-37% more; not covered. The present-cost ceilings follow the same rule
 * across 3 captures with the panel clock pinned, 2026-09-17, build
 * 8e32ebfe3968-diag. */

static void
perf_guard(const char* name, int64_t measured_us, int64_t ceiling_us) {
#if CONFIG_LAUNCHER_QEMU
    /* A ceiling pegged on the board prices the board's clock, not an
     * emulator's, so there it is reported and not enforced. */
    ESP_LOGI("device_tests", "%s: %lld us, board ceiling %lld us not enforced", name, (long long)measured_us,
             (long long)ceiling_us);
    return;
#endif
    TEST_ASSERT_LESS_THAN_MESSAGE((int)ceiling_us, (int)measured_us, name);
}

static void
perf_target(const char* name, int64_t measured_us, int64_t goal_us, int64_t ceiling_us) {
    const int64_t distance_percent = ((measured_us - goal_us) * 100) / goal_us;

    ESP_LOGI("device_tests", "PERF TARGET %s: measured %lld us, goal %lld us, distance %+lld%%", name,
             (long long)measured_us, (long long)goal_us, (long long)distance_percent);
    if (measured_us > goal_us) {
        perf_unmet_targets++;
    }
    if (!gas_ab_reporting) {
        perf_guard(name, measured_us, ceiling_us);
    }
}

/* The worst case: every cell on the screen moving at once. Unrelated code
 * shifting the flash layout can move this row between builds with no work
 * changed, so check the control rows before reading a miss here as a
 * regression. */
#define FULL_STEP_BUDGET_US 5800

/* Every frame budget in this file targets measured * 0.9, rounded - a
 * fixed 10% demand, not a ceiling matching whatever the code costs today;
 * the measured number beside each assertion is the anchor, the target is a
 * tenth under it. Some budgets read higher than an older number because the
 * older one predates now-added features (temperature, viscosity, drag,
 * percolation) - holding it frozen would conflate a feature's real cost
 * with a regression. This one: measured 6434 us -> target 5800. */

/* Half full, and deliberately not settled: a grid of falling grains is the
 * expensive case, because every one of them attempts a move. A settled
 * pile is cheaper and would flatter the measurement. */
static void
build_full_size_step_scene(sand_t* real, uint8_t* big) {
    sand_init(real, big, REAL_W, REAL_H, 99u);

    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (((x + y) & 1) == 0) {
                sand_set(real, x, y, SAND_FIRST_SHADE);
            }
        }
    }
}

static void
test_a_full_size_step_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(big, "the real grid must fit in what the framebuffer leaves behind");

    sand_t real;
    build_full_size_step_scene(&real, big);
    const int grains = sand_count(&real);

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "sand_step on %dx%d with %d grains: %lld us", REAL_W, REAL_H, grains, (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(grains, sand_count(&real), "the full-size grid must conserve grains too");

    free(big);

    perf_target("full-size step", per_step, FULL_STEP_BUDGET_US, 7432);
}

static void
build_water_scene(sand_t* real, uint8_t* big, uint8_t* blocks) {
    sand_init(real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(real, blocks);

    /* Half a screen of water, dropped in as an uneven slab so it is genuinely
     * flowing rather than already settled - the expensive case. */
    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = REAL_W / 4; x < (REAL_W * 3) / 4; x++) {
            sand_set(real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

static int64_t
water_scene_us_per_step(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    build_water_scene(&real, big, blocks);

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    free(big);
    free(blocks);
    return per_step;
}

static void
test_a_screen_of_water_fits_in_the_frame_budget(void) {
    /* Measured separately from sand, because water takes an entirely different
     * path through the step - and the one part of it that is not local, the
     * search across the flow, runs per cell. Something has to watch that. */
    const int64_t per_step = water_scene_us_per_step();

    ESP_LOGI("device_tests", "water flowing on %dx%d: %lld us per step", REAL_W, REAL_H, (long long)per_step);

    /* Water gets a larger budget than the full-step case: it moves an amount
     * rather than a cell, and takes a second sweep across the flow (the only
     * reason a tilted pool levels at all). This is the transient cost of a
     * screen-wide collapse - water at rest is 45 us; if this cost becomes
     * sustained, argue the budget down instead of up. Re-pegged perf-scoped:
     * measured 10743 -> target 9600. */
    perf_target("screen-wide water collapse", per_step, 9600, 13780);
}

#ifdef DEVICE_BUILD
static void
build_fire_scene(sand_t* real, uint8_t* big, uint8_t* blocks) {
    sand_init(real, big, REAL_W, REAL_H, 19u);
    sand_enable_sleeping(real, blocks);

    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(real, x, y, FIRE);
        }
    }
}

/* FEWER WARMUP STEPS THAN WATER, deliberately: fire BURNS OUT. Ten steps of
 * warmup would time a board that has already decayed to smoke and empty, which
 * is not the expensive case anyone is trying to fix. Two keeps it alight. */
#define FIRE_WARMUP_STEPS 2
#define FIRE_REPEATS      2
#endif /* DEVICE_BUILD */

#endif /* DEVICE_BUILD */

#ifdef DEVICE_BUILD
static void
test_the_gas_random_walk_against_the_exhaustive_mover(void) {
    int64_t best[2] = {-1, -1};

    const two_core_scope_t core = two_core_scope_begin(true);
    for (int arm = 0; arm < 2; arm++) {
        for (int r = 0; r < FIRE_REPEATS; r++) {
            uint8_t* big = malloc(REAL_W * REAL_H);
            uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
            TEST_ASSERT_NOT_NULL(big);
            TEST_ASSERT_NOT_NULL(blocks);

            sand_t real;
            build_fire_scene(&real, big, blocks);
            for (int i = 0; i < FIRE_WARMUP_STEPS; i++) {
                sand_step(&real, 0, 1000, 0);
            }

            sand_set_gas_walk(&real, arm == 1);
            const int64_t start = esp_timer_get_time();
            sand_step(&real, 0, 1000, 0);
            const int64_t took = esp_timer_get_time() - start;

            free(big);
            free(blocks);
            if (best[arm] < 0 || took < best[arm]) {
                best[arm] = took;
            }
        }
    }
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "gas mover, fire scene: exhaustive %lld us", (long long)best[0]);
    ESP_LOGI("device_tests", "gas mover, fire scene: random walk %lld us", (long long)best[1]);
    if (best[0] > 0) {
        ESP_LOGI("device_tests", "gas mover, fire scene: walk is %lld%% of the exhaustive cost",
                 (long long)((best[1] * 100) / best[0]));
    }
}

/* A/B against the plain serial step, same shape as the gas mover
 * comparison above. `gy` lets an arm fall fresh rather than flip a
 * settled pile - the chunk sweep this switch parallelises has
 * nothing to split once a board is asleep. No budget asserted. */
static void
time_two_core_arm(void (*build)(sand_t*, uint8_t*, uint8_t*), int gy, bool two_core, int64_t* out_per_step) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    uint8_t* stamps = malloc(sand_step_stamp_bytes(REAL_W, REAL_H));
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(stamps);

    sand_t real;
    build(&real, big, blocks);
    /* Without these the split path is never ready and both arms time the
     * serial walk, so the comparison reads as no gain from a second core. */
    sand_enable_step_stamps(&real, stamps);
    void* scratch = lane_scratch_open(&real);

    const two_core_scope_t core = two_core_scope_begin(two_core);
    const int steps = 20;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, gy, 0);
    }
    *out_per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);
    collect_core1_lane();

    free(scratch);
    free(stamps);
    free(big);
    free(blocks);
}

static void
report_two_core_ab(const char* scene, void (*build)(sand_t*, uint8_t*, uint8_t*), int gy) {
    int64_t serial_per_step = 0, two_core_per_step = 0;
    time_two_core_arm(build, gy, false, &serial_per_step);
    time_two_core_arm(build, gy, true, &two_core_per_step);

    ESP_LOGI("device_tests",
             "%s scene, %dx%d: serial %lld us/step, two-core %lld us/step "
             "(%lld%% of serial)",
             scene, REAL_W, REAL_H, (long long)serial_per_step, (long long)two_core_per_step,
             serial_per_step > 0 ? (long long)((two_core_per_step * 100) / serial_per_step) : 0);
}

/* build_full_size_step_scene() takes no blocks buffer; the sand-only arm
 * needs one wired anyway, since settled_bit still gates a chunk's work
 * under two-core stepping the same as it does serial. */
static void
build_full_size_step_scene_sleeping(sand_t* real, uint8_t* big, uint8_t* blocks) {
    build_full_size_step_scene(real, big);
    sand_enable_sleeping(real, blocks);
}

static void
test_two_core_step_against_the_serial_path_on_three_scenes(void) {
    report_two_core_ab("mixed flip", build_mixed_gravity_flip_scene, -1000);
    report_two_core_ab("water", build_water_scene, 1000);
    report_two_core_ab("sand-only", build_full_size_step_scene_sleeping, 1);
}

typedef struct {
    const char* name;
    int cell;
} quality_grid_t;

static void
build_quality_water_pour_scene(sand_t* real, uint8_t* big, uint8_t* blocks, int w, int h) {
    sand_init(real, big, w, h, 11u);
    sand_enable_sleeping(real, blocks);

    for (int y = 0; y < h / 2; y++) {
        for (int x = w / 4; x < (w * 3) / 4; x++) {
            sand_set(real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

/* A scene the bench builds at any grid size. The split covers the sweep, the
 * liquid cross-flow, the gas walk and a reacting cell's local rules;
 * impulses and the long-reach triggers stay serial, so how much of a step
 * the split passes hold is the ceiling on what a second core can buy for
 * that workload. */
typedef void (*quality_scene_fn)(sand_t* real, uint8_t* big, uint8_t* blocks, int w, int h);

typedef struct {
    int64_t per_step_us;
    int64_t parallel_us;
    int64_t total_us;
    int chunks;
} quality_bench_t;

static void
build_quality_sand_pour_scene(sand_t* real, uint8_t* big, uint8_t* blocks, int w, int h) {
    sand_init(real, big, w, h, 11u);
    sand_enable_sleeping(real, blocks);

    for (int y = 0; y < h / 2; y++) {
        for (int x = w / 4; x < (w * 3) / 4; x++) {
            sand_set(real, x, y, SAND_FIRST_SHADE);
        }
    }
}

static void
build_quality_gas_scene(sand_t* real, uint8_t* big, uint8_t* blocks, int w, int h) {
    sand_init(real, big, w, h, 17u);
    sand_enable_sleeping(real, blocks);

    for (int y = h / 2; y < h; y++) {
        for (int x = 0; x < w; x++) {
            sand_set(real, x, y, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
        }
    }
}

static void
build_quality_fire_scene(sand_t* real, uint8_t* big, uint8_t* blocks, int w, int h) {
    build_quality_gas_scene(real, big, blocks, w, h);
    sand_set(real, 0, h / 2, FIRE);
}

static void
build_quality_mixed_scene(sand_t* real, uint8_t* big, uint8_t* blocks, int w, int h) {
    sand_init(real, big, w, h, 23u);
    sand_enable_sleeping(real, blocks);

    for (int y = 0; y < h / 3; y++) {
        for (int x = 0; x < w / 2; x++) {
            sand_set(real, x, y, SAND_FIRST_SHADE);
        }
        for (int x = w / 2; x < w; x++) {
            sand_set(real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    for (int y = (h * 2) / 3; y < h; y++) {
        for (int x = w / 4; x < (w * 3) / 4; x++) {
            sand_set(real, x, y, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
        }
    }
}

static quality_bench_t
time_two_core_quality_scene(const quality_grid_t* quality, quality_scene_fn build, bool two_core) {
    const int w = GFX_WIDTH / quality->cell;
    const int h = GFX_HEIGHT / quality->cell;
    const size_t block_count =
        (size_t)((w + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * (size_t)((h + SAND_BLOCK_H - 1) / SAND_BLOCK_H);
    uint8_t* big = malloc((size_t)w * (size_t)h);
    uint8_t* blocks = malloc(block_count);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    uint8_t* stamps = malloc(sand_step_stamp_bytes(w, h));
    TEST_ASSERT_NOT_NULL(stamps);

    sand_t real;
    build(&real, big, blocks, w, h);
    /* Same reason as time_two_core_arm() above: no stamps and no lane
     * scratch means no split path to time. */
    sand_enable_step_stamps(&real, stamps);
    void* scratch = lane_scratch_open(&real);

    quality_bench_t out = {0};
    const two_core_scope_t core = two_core_scope_begin(two_core);
    const int steps = 20;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
        out.parallel_us += real.pass_us.sweep_us + real.pass_us.liquid_us + real.pass_us.float_us + real.pass_us.gas_us;
        out.total_us += real.pass_us.sweep_us + real.pass_us.liquid_us + real.pass_us.float_us + real.pass_us.gas_us
                        + real.pass_us.reactions_us + real.pass_us.impulses_us;
    }
    out.per_step_us = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);
    collect_core1_lane();

    out.chunks = sand_chunk_rows(&real) * sand_chunk_cols(&real);
    free(scratch);
    free(stamps);
    free(big);
    free(blocks);
    return out;
}

static void
report_quality_scene(const char* scene, const quality_grid_t* quality, quality_scene_fn build) {
    const quality_bench_t serial = time_two_core_quality_scene(quality, build, false);
    const quality_bench_t split = time_two_core_quality_scene(quality, build, true);
    const long long ratio = serial.per_step_us > 0 ? (split.per_step_us * 100) / serial.per_step_us : 0;
    const long long share = serial.total_us > 0 ? (serial.parallel_us * 100) / serial.total_us : 0;

    ESP_LOGI("device_tests",
             "TWO_CORE_WORKLOAD %s %s grid %dx%d chunks %d one-core %lld us two-core %lld us ratio %lld%% "
             "splittable %lld%%",
             scene, quality->name, GFX_WIDTH / quality->cell, GFX_HEIGHT / quality->cell, serial.chunks,
             (long long)serial.per_step_us, (long long)split.per_step_us, ratio, share);
}

static void
test_two_core_step_at_every_quality_grid_size(void) {
    static const quality_grid_t qualities[] = {
        {"ULTRA", 2}, {"HIGH", 3}, {"NORMAL", 4}, {"LOW", 6}, {"VERY LOW", 8},
    };

    static const struct {
        const char* name;
        quality_scene_fn build;
    } scenes[] = {
        {"water-pour", build_quality_water_pour_scene},
        {"sand-pour", build_quality_sand_pour_scene},
        {"gas", build_quality_gas_scene},
        {"fire", build_quality_fire_scene},
        {"mixed", build_quality_mixed_scene},
    };

    for (size_t si = 0; si < sizeof scenes / sizeof scenes[0]; si++) {
        for (size_t qi = 0; qi < sizeof qualities / sizeof qualities[0]; qi++) {
            report_quality_scene(scenes[si].name, &qualities[qi], scenes[si].build);
        }
    }
}

/* --- the chunk layout sweep ---------------------------------------------- *
 *
 * One machine-readable line per (quality, side pair, scene, orientation,
 * arm). Registered on request, one suite per quality, because five instances
 * of an emulator is how the grid gets measured in an evening and because no
 * boot of any image should pay for it uninvited.
 *
 * Under --icount a "us per step" line times 1000 is instructions per step,
 * not microseconds. The sweep RANKS layouts; it prices nothing.
 */

#define SWEEP_STEPS 12
#define SWEEP_SIDES 5
#define SWEEP_SEED  11u

typedef struct {
    const char* name;
    int gx, gy;
} sweep_orient_t;

static const sweep_orient_t sweep_orients[] = {{"landscape", 1000, 0}, {"portrait", 0, 1000}};

typedef struct {
    const char* name;
    int (*build)(sand_t*);
} sweep_scene_t;

static const sweep_scene_t sweep_scenes[] = {
    {"mixed-flip", build_layout_mixed_flip_scene},         {"water", build_layout_water_scene},
    {"sand-only", build_layout_sand_only_scene},           {"settling-pile", build_layout_settling_pile_scene},
    {"levelling-pool", build_layout_levelling_pool_scene},
};

typedef struct {
    const char* name;
    int w, h;
    int sides[SWEEP_SIDES][2]; /* the host pre-filter's shortlist, then the
                                * side that ships - a {0, 0} entry ends it */
} sweep_quality_t;

typedef struct {
    sand_t s;
    uint8_t* cells;
    uint8_t* blocks;
    uint8_t* stamps;
    void* scratch;
} sweep_board_t;

static void
sweep_board_open(sweep_board_t* b, int w, int h) {
    const size_t blocks =
        (size_t)((w + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * (size_t)((h + SAND_BLOCK_H - 1) / SAND_BLOCK_H);

    b->cells = malloc((size_t)w * (size_t)h);
    b->blocks = malloc(blocks);
    b->stamps = malloc(sand_step_stamp_bytes(w, h));
    TEST_ASSERT_NOT_NULL(b->cells);
    TEST_ASSERT_NOT_NULL(b->blocks);
    TEST_ASSERT_NOT_NULL(b->stamps);

    sand_init(&b->s, b->cells, w, h, SWEEP_SEED);
    sand_enable_sleeping(&b->s, b->blocks);
    sand_enable_step_stamps(&b->s, b->stamps);
    b->scratch = lane_scratch_open(&b->s);
}

static void
sweep_board_close(sweep_board_t* b) {
    free(b->scratch);
    free(b->stamps);
    free(b->blocks);
    free(b->cells);
}

/* The scene is painted with the split off, so both arms start on the same
 * board; the warm-up runs in the arm, because a board settled by one lane is
 * not the board two lanes settle and that difference is part of what is
 * being measured. */
/* THE THREE ARMS, and why there are three. An instruction count sums both
 * cores, so the two-lane arm is charged for whatever the other core does
 * while it waits - a bounded spin, or its idle task. It can never show a
 * win, whatever the layout. The one-thread walk of the same order can: it
 * is what the chunking costs, and the host pre-filter says how much of it
 * two lanes overlap. */
typedef enum {
    SWEEP_ARM_SERIAL,
    SWEEP_ARM_SOLO,
    SWEEP_ARM_SPLIT,
} sweep_arm_t;

static const char* const sweep_arm_names[] = {"serial", "solo", "split"};

static void
sweep_cell(const sweep_quality_t* q, const sweep_scene_t* sc, const int* side, const sweep_orient_t* o,
           sweep_arm_t arm) {
    sweep_board_t b;

    sweep_board_open(&b, q->w, q->h);
    TEST_ASSERT_TRUE_MESSAGE(sand_chunk_side_for_test(side[0], side[1]), "every swept side must clear the floor");
    const int warm = sc->build(&b.s);
    const int chunks = sand_chunk_cols(&b.s) * sand_chunk_rows(&b.s);

    const two_core_scope_t core = two_core_scope_begin(arm != SWEEP_ARM_SERIAL);
    sand_chunk_pass_set_driver_for_test(arm == SWEEP_ARM_SOLO ? SAND_CHUNK_PASS_SOLO : SAND_CHUNK_PASS_CORE1);
    for (int i = 0; i < warm; i++) {
        sand_step(&b.s, o->gx, o->gy, 0);
    }
    b.s.split_lane_aborts = 0;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < SWEEP_STEPS; i++) {
        sand_step(&b.s, o->gx, o->gy, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / SWEEP_STEPS;
    sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);
    two_core_scope_end(core);
    collect_core1_lane();

    ESP_LOGI("device_tests",
             "CHUNK_SWEEP quality=%s grid=%dx%d side=%dx%d scene=%s orient=%s arm=%s us_per_step=%lld aborts=%u "
             "chunks=%d",
             q->name, q->w, q->h, side[0], side[1], sc->name, o->name, sweep_arm_names[arm], (long long)per_step,
             b.s.split_lane_aborts, chunks);

    (void)sand_chunk_side_for_test(0, 0);
    sweep_board_close(&b);
}

static void
sweep_quality(const sweep_quality_t* q) {
    for (int si = 0; si < (int)(sizeof sweep_scenes / sizeof sweep_scenes[0]); si++) {
        for (int di = 0; di < SWEEP_SIDES && q->sides[di][0] != 0; di++) {
            for (int oi = 0; oi < (int)(sizeof sweep_orients / sizeof sweep_orients[0]); oi++) {
                for (int a = 0; a <= (int)SWEEP_ARM_SPLIT; a++) {
                    sweep_cell(q, &sweep_scenes[si], q->sides[di], &sweep_orients[oi], (sweep_arm_t)a);
                }
            }
        }
    }
    ESP_LOGI("device_tests", "CHUNK_SWEEP_COMPLETE quality=%s", q->name);
}

/* The shortlists main/apps/sand/tools/report_chunk_layout.sh produced, each
 * followed by the side the shipped rule picks for that grid. */
static void
test_chunk_sweep_ultra(void) {
    static const sweep_quality_t q = {"ULTRA", 184, 224, {{32, 36}, {47, 17}, {47, 36}, {62, 17}, {64, 64}}};
    sweep_quality(&q);
}

static void
test_chunk_sweep_high(void) {
    static const sweep_quality_t q = {"HIGH", 122, 149, {{25, 17}, {34, 17}, {25, 28}, {17, 28}, {42, 42}}};
    sweep_quality(&q);
}

static void
test_chunk_sweep_normal(void) {
    static const sweep_quality_t q = {"NORMAL", 92, 112, {{22, 24}, {17, 24}, {22, 17}, {17, 17}, {32, 32}}};
    sweep_quality(&q);
}

static void
test_chunk_sweep_low(void) {
    static const sweep_quality_t q = {"LOW", 61, 74, {{17, 17}, {19, 17}, {21, 17}, {22, 17}, {21, 21}}};
    sweep_quality(&q);
}

static void
test_chunk_sweep_very_low(void) {
    static const sweep_quality_t q = {"VERY LOW", 46, 56, {{17, 17}, {18, 17}, {19, 17}, {20, 17}, {0, 0}}};
    sweep_quality(&q);
}

static void
run_chunk_sweep_ultra_suite(void) {
    RUN_TEST(test_chunk_sweep_ultra);
}

static void
run_chunk_sweep_high_suite(void) {
    RUN_TEST(test_chunk_sweep_high);
}

static void
run_chunk_sweep_normal_suite(void) {
    RUN_TEST(test_chunk_sweep_normal);
}

static void
run_chunk_sweep_low_suite(void) {
    RUN_TEST(test_chunk_sweep_low);
}

static void
run_chunk_sweep_very_low_suite(void) {
    RUN_TEST(test_chunk_sweep_very_low);
}

SUITE_REGISTER_ON_REQUEST(run_chunk_sweep_ultra_suite);
SUITE_REGISTER_ON_REQUEST(run_chunk_sweep_high_suite);
SUITE_REGISTER_ON_REQUEST(run_chunk_sweep_normal_suite);
SUITE_REGISTER_ON_REQUEST(run_chunk_sweep_low_suite);
SUITE_REGISTER_ON_REQUEST(run_chunk_sweep_very_low_suite);

static void
test_a_screen_of_settled_sand_costs_almost_nothing(void) {
    /* The user-visible complaint this answers: adding lots of sand dropped the
     * framerate, even though most of it was just sitting there. */
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 5u);
    sand_enable_sleeping(&real, blocks);

    /* Every cell full, so nothing can move anywhere. */
    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, SAND_FIRST_SHADE);
        }
    }
    sand_step(&real, 0, 1, 0); /* one step to notice it is settled */

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 50;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "settled %dx%d grid: %lld us per step", REAL_W, REAL_H, (long long)per_step);

    const int grains = sand_count(&real);
    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(REAL_W * REAL_H, grains, "and nothing may have moved");
    /* The settled skip's 52-us reduction goal remains unmet at 128 us on
     * S3. The residual is unattributed; its regression ceiling must not
     * replace the goal. */
    perf_target("settled sand", per_step, 52, 133);
}

static void
test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget(void) {
    /* Worst case pouring: all blocks wake at once. */
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 13u);
    sand_enable_sleeping(&real, blocks);

    /* A big pour: the middle half of the screen's width, filled from the
     * floor up to half the screen's height - wide enough to span many
     * block-columns, deliberately not the whole grid. */
    for (int y = REAL_H / 2; y < REAL_H; y++) {
        for (int x = REAL_W / 4; x < (REAL_W * 3) / 4; x++) {
            sand_set(&real, x, y, SAND_FIRST_SHADE);
        }
    }
    const int grains = sand_count(&real);

    /* Let it fully settle first - every block should go to sleep, the
     * same state a real pile reaches between pours. */
    for (int i = 0; i < 300; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    /* Flip - straight up instead of straight down. */
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "gravity flip on a %d-grain pile, %dx%d: %lld us "
             "per step",
             grains, REAL_W, REAL_H, (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(grains, sand_count(&real), "flipping gravity must conserve grains too");

    free(big);
    free(blocks);

    perf_guard("settled-pile gravity flip", per_step, 9675);
}

/* Mass invariant for liquid scenes; water cell variant holds 1..15, diffusion
 * model adjusts amounts without changing cell count. */
static int
settled_pool_total_mass(const sand_t* s, int w, int h) {
    int total = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const cell_t c = sand_at(s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
                total += CELL_VARIANT(c);
            }
        }
    }
    return total;
}

/* A 90-degree turn is the expensive case a gravity reversal is not:
 * reversing drops the body in place, while turning sideways makes the pool
 * re-level across the full grid width, which is what the cross-flow search
 * costs the most for. Swept one step per degree because the expensive
 * frames are the mid-re-level ones. The worst step is logged alongside the
 * asserted mean, since a mean alone can hide a spike. */
static void
test_turning_a_settled_pool_to_landscape_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(&real, blocks);

    /* About 40% of the grid, full width, resting on the floor - the user's
     * own "fill the screen to about 40% with water in portrait". */
    for (int y = (REAL_H * 3) / 5; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
    const int mass = settled_pool_total_mass(&real, REAL_W, REAL_H);

    /* Settle until every block sleeps - "with the water settled" is half the
     * reported condition, and a pool that is still moving would time
     * something else entirely. */
    for (int i = 0; i < 300; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    /* THE TURN. */
    const two_core_scope_t core = two_core_scope_begin(true);
    const int steps = 90;
    int64_t worst = 0;
    const int64_t start = esp_timer_get_time();
    for (int i = 1; i <= steps; i++) {
        const int gx = (1000 * i) / steps;
        const int gy = 1000 - gx;
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, gx, gy, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "portrait->landscape turn on a settled %d-mass "
             "pool, %dx%d: %lld us per step, worst single "
             "step %lld us",
             mass, REAL_W, REAL_H, (long long)per_step, (long long)worst);

    const int mass_after = settled_pool_total_mass(&real, REAL_W, REAL_H);

    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(mass, mass_after,
                                  "turning the board must move water, not create or destroy it - the "
                                  "cell COUNT changes as the pool re-levels, the mass must not");

    /* MEASURED 9,763 us per step on device, perf-scoped, after the block
     * narrowed to 16x32. Budget is that x 0.9 rounded DOWN to 8,700. */

    /* THE 14000 THIS REPLACES WAS NEVER A BUDGET - it was borrowed from
     * the water screen so the row would compile, and said so. It also
     * misled a reader into reporting a 167% regression that never
     * happened, by dividing it by 0.9 as if it were pegged. */

    /* WORTH KNOWING BEFORE OPTIMISING THIS ROW: the impulse flight pass
     * never runs here at all - s->impulse_count is 0 for all 390 steps,
     * host-counted 2026-09-06 - and a host pass map puts ~48% of the cost
     * in cross-flow, ~1% reactions, ~1.5% gas. */
    perf_target("settled pool landscape turn", per_step, 8700, 7423);
}

/* Tilt shape uses exponential moving average with tau interpolating between
 * TILT_TAU_MOVING_MS and TILT_TAU_STILL_MS. Moving tau prioritizes demanding
 * case with largest gravity delta. Returns mean and worst single step. */
static int64_t
time_a_quarter_turn(sand_t* real, int steps, int64_t* worst_out) {
    const int dt_ms = 24;
    const int tau_ms = TILT_TAU_MOVING_MS;

    int32_t gx_q8 = 0;
    int32_t gy_q8 = 1000 * 256;

    int64_t worst = 0;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        gx_q8 += (int32_t)(((int64_t)(1000 * 256 - gx_q8) * dt_ms) / (tau_ms + dt_ms));
        gy_q8 += (int32_t)(((int64_t)(0 - gy_q8) * dt_ms) / (tau_ms + dt_ms));

        const int64_t t0 = esp_timer_get_time();
        sand_step(real, gx_q8 / 256, gy_q8 / 256, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    *worst_out = worst;
    return per_step;
}

/* Processes one row of the wood/leaf shading walk: adds tint work into
 * *sink for every tinted cell and reports whether the row lit at least one
 * gust-wake cell. On the first full pass (rep == 0) it also folds that
 * row's wood/leaf/near-leaf counts into the running totals. */
static int
wood_leaf_row_pass(const uint8_t* row, const uint8_t* above, const uint8_t* below, int w, int y, int rep,
                   const int8_t top5[5][2], unsigned* sink, int* wood, int* leaf, int* near_leaf) {
    int lit = 0;
    for (int x = 0; x < w; x++) {
        const unsigned hash = material_grain_hash(x, y);
        const bool is_leaf = row[x] == MATX(MATX_LEAF);
        const bool tinted =
            is_leaf
            || (row[x] == CELL_MAKE(MAT_WOOD, 0) && material_wood_near_leaf(above, row, below, x, w, top5, hash, 5u));
        if (tinted) {
            *sink += material_wood_leaf_wave(rep * 40u, x, w, hash);
            lit = 1;
        }
        if (rep == 0) {
            if (is_leaf) {
                (*leaf)++;
            } else if (row[x] == CELL_MAKE(MAT_WOOD, 0)) {
                (*wood)++;
                if (tinted) {
                    (*near_leaf)++;
                }
            }
        }
    }
    return lit;
}

/* Row y's cell pointer plus its above/below neighbours (NULL past either
 * edge) for wood_leaf_row_pass(). */
static void
wood_leaf_row_window(const uint8_t* big, int w, int h, int y, const uint8_t** row, const uint8_t** above,
                     const uint8_t** below) {
    *row = big + (size_t)y * w;
    *above = (y > 0) ? *row - w : NULL;
    *below = (y < h - 1) ? *row + w : NULL;
}

/* paint_row_n() is `static inline` inside app_sand.c and unreachable from
 * here, so no row in this suite exercises the app's paint path - the
 * shading could land measuring "nothing" because nothing was looking. This
 * times the per-cell work the shading adds, over the real grid, walked the
 * way paint_row_n() walks it: the scan and the wave, not the framebuffer
 * writes nor the extra paint_row() calls the gust's wake tick causes.
 * Prints; asserts no budget, since half the cost is out of reach. */
static void
test_the_wood_leaf_shading_on_a_grove(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    TEST_ASSERT_NOT_NULL(big);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 3u);
    build_tree_grove_scene(&real);

    int8_t top5[5][2];
    int down = 0;
    material_wood_leaf_top5(0, 1000, &down, top5);

    int wood = 0, leaf = 0, near_leaf = 0, rows_lit = 0;
    unsigned sink = 0;

    const int64_t start = esp_timer_get_time();
    for (int rep = 0; rep < 20; rep++) {
        for (int y = 0; y < REAL_H; y++) {
            const uint8_t* row;
            const uint8_t* above;
            const uint8_t* below;
            wood_leaf_row_window(big, REAL_W, REAL_H, y, &row, &above, &below);
            const int lit =
                wood_leaf_row_pass(row, above, below, REAL_W, y, rep, top5, &sink, &wood, &leaf, &near_leaf);
            if (rep == 0) {
                rows_lit += lit;
            }
        }
    }
    const int64_t per_pass = (esp_timer_get_time() - start) / 20;

    const int64_t c0 = esp_timer_get_time();
    for (int rep = 0; rep < 20; rep++) {
        for (int y = 0; y < REAL_H; y++) {
            const uint8_t* row = big + (size_t)y * REAL_W;
            for (int x = 0; x < REAL_W; x++) {
                sink += material_grain_hash(x, y);
                sink += (row[x] == MATX(MATX_LEAF)) || (row[x] == CELL_MAKE(MAT_WOOD, 0));
            }
        }
    }
    const int64_t control_pass = (esp_timer_get_time() - c0) / 20;

    ESP_LOGI("device_tests",
             "wood/leaf shading on a grove: %lld us per full-grid pass, "
             "control %lld us, so the shading is %lld us "
             "(wood %d, of which %d beside a leaf; leaf %d; %d of %d rows "
             "carry foliage and so wake every gust tick) [%u]",
             (long long)per_pass, (long long)control_pass, (long long)(per_pass - control_pass), wood, near_leaf, leaf,
             rows_lit, REAL_H, sink & 1u);

    free(big);
}

/* Reported from play: the frame drops when water is poured onto the bed;
 * pacing is stable once plants are merely drinking from wet dirt. Same bed
 * and settle, timed over the steps immediately AFTER a fresh pour against a
 * quiet window, so the pair brackets what a player sees - the DIFFERENCE
 * between the two rows is the point, a number from either alone describes
 * half the experience. */
static void
test_pouring_water_onto_a_plant_bed_costs_more_than_steady_growth(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_plant_bed_scene(&real);
    for (int i = 0; i < PLANT_BED_SETTLE_STEPS; i++) {
        if (i == PLANT_BED_RAIN_A || i == PLANT_BED_RAIN_B) {
            plant_bed_rain(&real);
        }
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = 20;

    const two_core_scope_t core = two_core_scope_begin(true);

    /* Steady first, from the same board the pour will start from - measuring
     * the pour first would leave the steady rows a wetter bed than the one
     * the other row times. */
    int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t steady = (esp_timer_get_time() - start) / steps;

    plant_bed_rain(&real);
    start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t poured = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "plant bed pour: steady %lld us, first %d steps after a pour "
             "%lld us (%lld us more, %lld%%)",
             (long long)steady, steps, (long long)poured, (long long)(poured - steady),
             steady > 0 ? (long long)(((poured - steady) * 100) / steady) : 0);

    free(big);
    free(blocks);
}

static void
test_a_growing_plant_bed_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_plant_bed_scene(&real);

    for (int i = 0; i < PLANT_BED_SETTLE_STEPS; i++) {
        if (i == PLANT_BED_RAIN_A || i == PLANT_BED_RAIN_B) {
            plant_bed_rain(&real);
        }
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = 20;
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "growing plant bed, %dx%d: %lld us per step", REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* RED ON PURPOSE, reduction target, not regression guard. Soak/dry is 28%
     * of this step. Re-pegged perf-scoped: measured 63,397 -> target 57,000. */
    perf_target("growing plant bed", per_step, 57000, 55093);
}

static void
test_a_campfire_on_a_sand_bed_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 23u);
    sand_enable_sleeping(&real, blocks);

    build_campfire_scene(&real);

    /* Let the sand settle and the fire catch, so the timed steps are a
     * burning campfire rather than a scene still falling into place. */
    for (int i = 0; i < 30; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = 20;
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "campfire on a sand bed, %dx%d: %lld us per step", REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* MEASURED 35,963 us per step on device, perf-scoped, after the block
     * narrowed to 16x32. Budget is that x 0.9 = 32,366, rounded DOWN to
     * 32,300 so the target is never looser than the convention. */
    perf_target("campfire on sand", per_step, 32300, 31566);
}

/* A tilted board is a different path, not a rotation of the same one:
 * equalise_gas() takes its spread direction from ring_dir(i_stable + 2), and
 * gas_run_t's carry runs only where py == 0. Packed bounds the worst case
 * and is the only shape that fires the row skip; the half-screen scene below
 * is the realistic counterpart, and the pair is the point. */
static void
test_turning_a_packed_screen_of_gas_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 31u);
    sand_enable_sleeping(&real, blocks);

    build_smoke_and_steam_scene(&real);
    const int total = REAL_W * REAL_H;

    int64_t worst = 0;
    const int64_t per_step = time_a_quarter_turn(&real, 24, &worst);

    ESP_LOGI("device_tests",
             "quarter turn on a PACKED screen of gas, %dx%d, %s: "
             "%lld us per step, worst single step %lld us, last gas pass %lld us",
             REAL_W, REAL_H, sand_two_core_step_enabled() ? "two-core" : "serial", (long long)per_step,
             (long long)worst, (long long)real.pass_us.gas_us);

    /* Read before the frees, asserted after - Unity longjmps out of a failing
     * assert, so an assert ahead of free() would leak ~41 KB on this device's
     * no-PSRAM heap. */
    const int count = sand_count(&real);

    free(big);
    free(blocks);

    /* Same condensation caveat as the smoke-and-steam row above, and more
     * of it: a turning board keeps stirring steam into fresh 2x2 patches,
     * so the loss is larger here and varies run to run. */
    if (!gas_ab_reporting) {
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(total - total / 8, count,
                                                 "turning the board must not empty it - steam condensing into water "
                                                 "loses three cells a patch, but a packed screen that has shed an "
                                                 "eighth of itself is not the scene this row means to time");
    }
    perf_target("packed gas turn", per_step, 128800, 121348);
}

static void
test_turning_a_half_screen_of_gas_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 31u);
    sand_enable_sleeping(&real, blocks);

    /* 40% of the grid, full width, against the ceiling - where gas ends up. */
    for (int y = 0; y < (REAL_H * 2) / 5; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_GAS, 0));
        }
    }

    /* Settle first: the turn should start from a body at rest, not from a
     * field still finding its own shape. */
    for (int i = 0; i < 60; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int before = sand_count(&real);

    int64_t worst = 0;
    const int64_t per_step = time_a_quarter_turn(&real, 24, &worst);

    ESP_LOGI("device_tests",
             "quarter turn on a HALF screen of gas, %dx%d, %s: "
             "%lld us per step, worst single step %lld us, last gas pass %lld us",
             REAL_W, REAL_H, sand_two_core_step_enabled() ? "two-core" : "serial", (long long)per_step,
             (long long)worst, (long long)real.pass_us.gas_us);

    const int after = sand_count(&real);

    free(big);
    free(blocks);

    if (!gas_ab_reporting) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(before, after,
                                      "turning the board must move gas, not create or destroy it - decay is "
                                      "off by default, so the cell count is conserved across the turn");
    }
    perf_target("half-screen gas turn", per_step, 46100, 45133);
}

static void
test_flipping_gravity_on_a_mixed_scene_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    build_mixed_gravity_flip_scene(&real, big, blocks);

    /* Flip - straight up instead of straight down. */
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "gravity flip on a mixed sand/water/stone-X "
             "scene, %dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    /* No grain-conservation check. Water's model can spread mass across
     * cells, so sand_count() legitimately changes; test_a_screen_of_water_
     * fits_in_the_frame_budget skips this same check for the same reason.
     * Asserting it here once leaked ~41 KB - the failure's longjmp skipped
     * the frees below it. */

    free(big);
    free(blocks);

    /* A deliberate reduction target from the day it was written (12000
     * against a then-measured 15144), never headroom. Re-pegged
     * 2026-09-11, perf-scoped: measured 9311 -> target 8300, from the
     * 12999 -> 11700 that had stopped asking for anything. */
    perf_target("mixed-scene gravity flip", per_step, 8300, 15044);
}

/* select/mask pairs from xtensa/xt_perf_consts.h. "insn" doubles as the
 * retired-instruction reference for the derived cycles-per-insn line below.
 * All confirmed present in this IDF; none were dropped. */
typedef struct {
    const char* name;
    uint16_t select;
    uint16_t mask;
} xtperf_event_t;

static const xtperf_event_t XTPERF_EVENTS[] = {
    {"insn", XTPERF_CNT_INSN, XTPERF_MASK_INSN_ALL},
    {"window", XTPERF_CNT_EXR, XTPERF_MASK_EXR_WINDOW},
    {"level1_int", XTPERF_CNT_EXR, XTPERF_MASK_EXR_LEVEL1_INT},
    {"replays", XTPERF_CNT_EXR, XTPERF_MASK_EXR_REPLAYS},
    {"icache_miss_stall", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_CACHE_MISS},
    {"iterative_mul", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ITERATIVE_MUL},
    {"iterative_div", XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ITERATIVE_DIV},
    {"d_stall_all", XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_ALL},
    {"bubbles_cti", XTPERF_CNT_BUBBLES, XTPERF_MASK_BUBBLES_CTI},
    {"bubbles_all", XTPERF_CNT_BUBBLES, XTPERF_MASK_BUBBLES_ALL},
    {"branch_taken", XTPERF_CNT_INSN, XTPERF_MASK_INSN_BRANCH_TAKEN},
    {"branch_not_taken", XTPERF_CNT_INSN, XTPERF_MASK_INSN_BRANCH_NOT_TAKEN},
    {"call", XTPERF_CNT_INSN, (uint16_t)(XTPERF_MASK_INSN_CALL | XTPERF_MASK_INSN_CALLX)},
    {"icache_miss_fetch", XTPERF_CNT_I_MEM, XTPERF_MASK_I_MEM_CACHE_MISSES},
    {"iram_fetch", XTPERF_CNT_I_MEM, XTPERF_MASK_I_MEM_IRAM},
};
#define XTPERF_EVENT_COUNT (sizeof(XTPERF_EVENTS) / sizeof(XTPERF_EVENTS[0]))

/* Counter 0 is cycles, seeded the way xtensa_perfmon_exec() seeds it (select
 * 0, mask 0xffff). kernelcnt 0 / tracelevel -1 is exec()'s own encoding for
 * "no interrupt-level filter" (xtensa_perfmon_config_t: negative tracelevel
 * means the filter is ignored) - every level counts, none excluded, which is
 * what a whole-step instrument needs. */
static void
measure_xtperf_event(const char* scene, sand_t* real, int gx, int gy, int gz, int steps, const xtperf_event_t* event,
                     uint32_t* out_cycles, uint32_t* out_value) {
    xtensa_perfmon_stop();
    xtensa_perfmon_init(0, XTPERF_CNT_CYCLES, 0xffff, 0, -1);
    xtensa_perfmon_init(1, event->select, event->mask, 0, -1);
    xtensa_perfmon_reset(0);
    xtensa_perfmon_reset(1);
    xtensa_perfmon_start();

    for (int i = 0; i < steps; i++) {
        sand_step(real, gx, gy, gz);
    }

    xtensa_perfmon_stop();
    const uint32_t cycles = xtensa_perfmon_value(0);
    const uint32_t value = xtensa_perfmon_value(1);
    const bool cycles_overflowed = xtensa_perfmon_overflow(0) != ESP_OK;
    const bool value_overflowed = xtensa_perfmon_overflow(1) != ESP_OK;

    ESP_LOGI("xtperf", "scene=%s event=%s cycles_per_step=%u value_per_step=%u steps=%d%s%s", scene, event->name,
             (unsigned)(cycles / (uint32_t)steps), (unsigned)(value / (uint32_t)steps), steps,
             cycles_overflowed ? " overflow=cycles" : "", value_overflowed ? " overflow=value" : "");

    if (event->select == XTPERF_CNT_INSN && event->mask == XTPERF_MASK_INSN_ALL && value != 0) {
        const uint32_t cpi_x100 = (uint32_t)(((uint64_t)cycles * 100) / value);
        ESP_LOGI("xtperf", "scene=%s cycles_per_retired_insn=%u.%02u", scene, (unsigned)(cpi_x100 / 100),
                 (unsigned)(cpi_x100 % 100));
    }

    *out_cycles = cycles;
    *out_value = value;
}

static void
run_xtperf_over_mixed_scene(uint64_t* total_cycles, uint64_t* total_insn) {
    for (size_t e = 0; e < XTPERF_EVENT_COUNT; e++) {
        uint8_t* big = malloc(REAL_W * REAL_H);
        uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
        TEST_ASSERT_NOT_NULL(big);
        TEST_ASSERT_NOT_NULL(blocks);

        sand_t real;
        build_mixed_gravity_flip_scene(&real, big, blocks);

        uint32_t cycles = 0, value = 0;
        measure_xtperf_event("mixed_flip", &real, 0, -1000, 0, 20, &XTPERF_EVENTS[e], &cycles, &value);

        free(big);
        free(blocks);

        *total_cycles += cycles;
        if (XTPERF_EVENTS[e].select == XTPERF_CNT_INSN && XTPERF_EVENTS[e].mask == XTPERF_MASK_INSN_ALL) {
            *total_insn += value;
        }
    }
}

static void
run_xtperf_over_water_scene(uint64_t* total_cycles, uint64_t* total_insn) {
    for (size_t e = 0; e < XTPERF_EVENT_COUNT; e++) {
        uint8_t* big = malloc(REAL_W * REAL_H);
        uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
        TEST_ASSERT_NOT_NULL(big);
        TEST_ASSERT_NOT_NULL(blocks);

        sand_t real;
        build_water_scene(&real, big, blocks);

        uint32_t cycles = 0, value = 0;
        measure_xtperf_event("water", &real, 0, 1000, 0, 20, &XTPERF_EVENTS[e], &cycles, &value);

        free(big);
        free(blocks);

        *total_cycles += cycles;
        if (XTPERF_EVENTS[e].select == XTPERF_CNT_INSN && XTPERF_EVENTS[e].mask == XTPERF_MASK_INSN_ALL) {
            *total_insn += value;
        }
    }
}

static void
run_xtperf_over_full_step_scene(uint64_t* total_cycles, uint64_t* total_insn) {
    for (size_t e = 0; e < XTPERF_EVENT_COUNT; e++) {
        uint8_t* big = malloc(REAL_W * REAL_H);
        TEST_ASSERT_NOT_NULL(big);

        sand_t real;
        build_full_size_step_scene(&real, big);

        uint32_t cycles = 0, value = 0;
        measure_xtperf_event("full_step", &real, 0, 1, 0, 10, &XTPERF_EVENTS[e], &cycles, &value);

        free(big);

        *total_cycles += cycles;
        if (XTPERF_EVENTS[e].select == XTPERF_CNT_INSN && XTPERF_EVENTS[e].mask == XTPERF_MASK_INSN_ALL) {
            *total_insn += value;
        }
    }
}

/* An instrument, not a gate: asserts only that the counters moved at all -
 * the logged ratios are the point. Rebuilds each scene per event so every
 * window starts from the same deterministic state rather than drifting
 * across fifteen back-to-back runs. Counters are per-CPU, so the core is
 * checked rather than assumed. */
static void
log_mixed_scene_hashes(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    if (big == NULL || blocks == NULL) {
        free(big);
        free(blocks);
        return;
    }
    sand_t real;
    build_mixed_gravity_flip_scene(&real, big, blocks);
    ESP_LOGI("xtperf", "scene=mixed_flip hash settled=%08" PRIx32, grid_hash(big, REAL_W * REAL_H));
    for (int i = 0; i < 20; i++) {
        sand_step(&real, 0, -1000, 0);
        if (i == 0 || i == 9 || i == 19) {
            ESP_LOGI("xtperf", "scene=mixed_flip hash flip%d=%08" PRIx32, i + 1, grid_hash(big, REAL_W * REAL_H));
        }
    }
    free(big);
    free(blocks);
}

static void
test_the_xtensa_counters_over_three_scenes(void) {
#if CONFIG_LAUNCHER_QEMU
    TEST_IGNORE_MESSAGE("QEMU does not model the performance monitor");
#endif
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, esp_cpu_get_core_id(),
                                  "perfmon counters are per-core; this instrument only means what it "
                                  "says if it counts and reads back on the same core the sand step "
                                  "actually runs on");

    uint64_t total_cycles = 0;
    uint64_t total_insn = 0;

    const two_core_scope_t core = two_core_scope_begin(true);
    log_mixed_scene_hashes();
    run_xtperf_over_mixed_scene(&total_cycles, &total_insn);
    run_xtperf_over_water_scene(&total_cycles, &total_insn);
    run_xtperf_over_full_step_scene(&total_cycles, &total_insn);
    two_core_scope_end(core);

    TEST_ASSERT_TRUE_MESSAGE(total_cycles > 0, "the cycle counter never moved across any scene or event");
    TEST_ASSERT_TRUE_MESSAGE(total_insn > 0, "the retired-instruction counter never moved across any scene");
}

/* Board banded with every material, reactive pairs touch, gravity inverted.
 * Catches combination costs. THE ASSERTION BELOW IS NOT A BUDGET. Replace
 * with real figure from `run_device_tests.sh`. */
static void
test_a_gravity_flip_on_every_material_at_once_stays_sane(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    impulse_t* impulses = malloc((size_t)ALL_PAIRS_IMPULSE_MAX * sizeof *impulses);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(impulses);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 23u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    /* Without this, sand_explode() has nowhere to write and the gunpowder
     * patches below can never detonate - see ALL_PAIRS_IMPULSE_MAX. */
    sand_enable_impulses(&real, impulses, ALL_PAIRS_IMPULSE_MAX);

    /* build_all_pairs_scene() (suite_sand_scenes.c) also plants the
     * deliberate gunpowder patches - the tiling alone scatters gunpowder
     * as one cell in nineteen, never enough to form the fuse's 2x2. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, ALL_PAIRS_SPAWN_COUNT,
                                         "the pattern below needs at least two materials to interleave");

    build_all_pairs_scene(&real);

    /* Let it get going - long enough for the reactions to be under way and
     * the liquids to have found their levels, so the flip lands on a live
     * scene rather than a freshly painted one. */
    for (int i = 0; i < 120; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, -1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "gravity flip with every material at once, "
             "%dx%d: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);
    free(impulses);

    /* THE 87800 BUDGET IS INVALIDATED, NOT CARRIED FORWARD: it was
     * measured against the fourteen-material scene, and this scene is
     * now bigger. */

    /* MEASURED 90,713 us per step, 2026-09-10 - this scene's first clean
     * capture, which is what the 200000 sanity ceiling before it was
     * waiting for. Budget is that x 0.9 rounded DOWN to 81,600. */
    perf_target("all-material gravity flip", per_step, 81600, 88630);
}

static void
test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(&real, blocks);

    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
        }
    }
    sand_set(&real, 0, 0, FIRE);
    const int total = REAL_W * REAL_H;

    const int64_t start = esp_timer_get_time();
    sand_step(&real, 0, 1000, 0);
    const int64_t elapsed = esp_timer_get_time() - start;

    ESP_LOGI("device_tests",
             "fire cascading through a full %dx%d screen of "
             "gas, %s: %lld us for the one step, gas pass %lld us",
             REAL_W, REAL_H, sand_two_core_step_enabled() ? "two-core" : "serial", (long long)elapsed,
             (long long)real.pass_us.gas_us);

    if (!gas_ab_reporting) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(total, sand_count(&real),
                                      "setup: cells must only ever convert material, never appear or "
                                      "vanish, across gas igniting into fire");
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(&real, REAL_W - 1, REAL_H - 1)),
                                      "setup: the cascade must have reached the far corner - the whole "
                                      "grid must have ignited in this one step, or this is not "
                                      "actually measuring the worst case it claims to");
    }

    free(big);
    free(blocks);

    /* A DELIBERATELY SYNTHETIC WORST CASE: not held to plain-material
     * budgets. Failing by design, not moving goalposts. */
    perf_target("full-screen gas cascade", elapsed, 222700, 196791);
}

static void
test_the_gas_budget_rows_on_the_serial_path(void) {
    gas_ab_reporting = true;
    two_core_scope_t core = two_core_scope_begin(false);
    test_turning_a_packed_screen_of_gas_fits_in_the_frame_budget();
    test_turning_a_half_screen_of_gas_fits_in_the_frame_budget();
    test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget();
    two_core_scope_end(core);
    core = two_core_scope_begin(true);
    test_turning_a_packed_screen_of_gas_fits_in_the_frame_budget();
    test_turning_a_half_screen_of_gas_fits_in_the_frame_budget();
    test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget();
    two_core_scope_end(core);
    gas_ab_reporting = false;
}

static void
test_a_full_screen_of_fire_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 19u);
    sand_enable_sleeping(&real, blocks);

    for (int y = 0; y < REAL_H; y++) {
        for (int x = 0; x < REAL_W; x++) {
            sand_set(&real, x, y, FIRE);
        }
    }
    const int total = REAL_W * REAL_H;

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "full %dx%d screen already fire, steady "
             "state: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, sand_count(&real),
                                  "setup: a fully packed screen of same-density fire cannot "
                                  "displace, ignite, or smother anything - the count must not "
                                  "drift");

    free(big);
    free(blocks);

    perf_guard("full-screen fire", per_step, 71189);
}

/* Four liquids of different density painted upside down
 * (build_four_liquid_scene(), shared with test_the_four_liquid_scene_
 * keeps_reacting_after_settling) so lava, acid, water and oil migrate past
 * each other the whole window instead of settling into inert bands. Also
 * the only benchmark here, besides the gravity-flip test above, running at
 * sand_set_mobility(SAND_MOBILITY_PER_MATERIAL) - the setting app_sand.c
 * itself uses - so this holds the app's own liquid path to any real
 * ceiling. */
static void
test_four_liquids_reacting_at_once_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 29u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_four_liquid_scene(&real);

    /* Settle first - the same "let it get going" step as the every-material
     * flip test above, so the measured window lands on a live scene. */
    for (int i = 0; i < 10; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "four liquids reacting at once, %dx%d: %lld "
             "us per step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* RE-PEGGED 2026-09-11, perf-scoped: 86,920 us measured, inside the
     * 89,200 it carried, so that number had stopped being a target.
     * x 0.9 rounded DOWN -> 78,200. */
    perf_target("four reacting liquids", per_step, 78200, 72791);
}

static void
test_the_lava_stress_scene_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 37u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_lava_stress_scene(&real);

    for (int i = 0; i < 30; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    int64_t pass_totals[6] = {0};
    int64_t pass_peak[6] = {0};
    int64_t peak_total = -1;
    int peak_impulses = 0;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
        const int64_t pass[6] = {real.pass_us.sweep_us, real.pass_us.liquid_us,    real.pass_us.float_us,
                                 real.pass_us.gas_us,   real.pass_us.reactions_us, real.pass_us.impulses_us};
        int64_t total = 0;
        for (int j = 0; j < 6; j++) {
            pass_totals[j] += pass[j];
            total += pass[j];
        }
        if (total > peak_total) {
            memcpy(pass_peak, pass, sizeof pass_peak);
            peak_total = total;
            peak_impulses = real.impulse_count;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "lava stress scene, %dx%d: %lld us per step", REAL_W, REAL_H, (long long)per_step);
    log_pass_split("lava stress scene", steps, real.impulse_max, pass_totals, pass_peak, peak_impulses,
                   real.impulse_cap_hits);

    free(big);
    free(blocks);

    /* RE-PEGGED 2026-09-11, perf-scoped: 106,354 us measured, inside the
     * 109,000 it carried. x 0.9 rounded DOWN -> 95,700. */
    perf_target("lava stress", per_step, 95700, 94778);
}

static void
test_a_screen_of_smoke_and_steam_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 31u);
    sand_enable_sleeping(&real, blocks);

    build_smoke_and_steam_scene(&real);
    const int total = REAL_W * REAL_H;

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "screen of smoke and steam, %dx%d: %lld us "
             "per step",
             REAL_W, REAL_H, (long long)per_step);

    /* Read before the frees below, asserted after - the same fix
     * test_a_gravity_flip_on_every_material_at_once_stays_sane documents:
     * Unity longjmps out of a failing assert, so an assert ahead of
     * free() would skip it and leak ~41 KB on this device's no-PSRAM
     * heap. */
    const int count = sand_count(&real);

    free(big);
    free(blocks);

    /* host twin forces off with sand_set_condenses() due to budget pegged
     * with condensation running. Screen did not quietly empty into unmeasured
     * state. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(total - total / 16, count,
                                             "setup: a screen of smoke and steam must still be essentially full "
                                             "at the end of the window - steam condensing into water loses three "
                                             "cells a patch, but losing an appreciable fraction of the board "
                                             "means it decayed into something else");
    /* RE-PEGGED 2026-09-10: 115,178 us measured, inside the 127000 it
     * carried. x 0.9 rounded DOWN -> 103,600. */
    perf_target("smoke and steam", per_step, 103600, 94053);
}

/* 480 glass compartments (build_thermal_shock_scene(), shared with
 * test_the_thermal_shock_scene_shatters_in_both_directions). No settling
 * steps: every ring starts strictly between the two shock thresholds and
 * touching from step 1, so the lattice is already at its most active the
 * moment it's painted. Clean measurement 98738 us -> target 89000
 * (file-wide 0.9 rule, see FULL_STEP_BUDGET_US's comment). */
static void
test_the_thermal_shock_scene_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    /* Step count is fixed at 10 by the host guard beside this test (its own
     * comment covers the cullet timeline); the ceiling is chosen against
     * the device's 5-second task watchdog at that fixed count - raising the
     * count without minding the ceiling needs re-doing the bet. */

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_thermal_shock_scene(&real);

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "thermal shock lattice, %dx%d: %lld us per "
             "step",
             REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    perf_target("thermal shock", per_step, 89000, 86932);
}

static void
test_the_boiler_scene_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 43u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_boiler_scene(&real);

    for (int i = 0; i < 20; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 30;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "boiler scene, %dx%d: %lld us per step", REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* RE-PEGGED 2026-09-11, perf-scoped: 28,125 us measured, inside the
     * 28,500 it carried. x 0.9 rounded DOWN -> 25,300. */
    perf_target("boiler", per_step, 25300, 28853);
}

/* Sand and dirt poured in equal amounts, water dropped over both until
 * it settles (build_wet_earth_scene(), shared with test_the_wet_earth_
 * scene_keeps_percolating_across_the_window). First benchmark to put
 * sustained load through sand_step_reactions() via moisture rather than
 * fire/heat (see the builder's own comment on may_have_moisture). 35
 * settle steps then 30 measured, matching the host test's own window (see
 * that test's comment for why 35). */
static void
test_the_wet_earth_scene_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    /* Measured 100367 us/30 steps -> target 80000 (measured * 0.8, NOT this
     * file's usual * 0.9 - an explicit instruction for this benchmark, not
     * an inconsistency to fix). */

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 53u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_wet_earth_scene(&real);

    for (int i = 0; i < 35; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 30;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "wet earth scene, %dx%d: %lld us per step", REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    /* Measured 59,824 perf-scoped, after the block narrowed to 16x32, x 0.8
     * rounded down - this row's own exception to the file-wide x 0.9. */
    perf_target("wet earth", per_step, 47800, 37115);
}

/* The water-over-lava scene from this file's own section above, run as a
 * frame-budget test. TWENTY STEPS, NO SETTLING - matching test_the_water_
 * over_lava_scene_reaches_the_quench_cooloff_and_burst_paths_it_claims
 * exactly, so what this times is a scene already proven to reach quench,
 * cool_off_chain() and the burst gate. No settling step: the scene is
 * already at its busiest the instant it's painted (the whole seam touching
 * for the first time). */
static void
test_the_water_over_lava_scene_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc((size_t)REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    impulse_t* impulses = malloc((size_t)WATER_LAVA_IMPULSE_MAX * sizeof *impulses);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(impulses);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 59u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, WATER_LAVA_IMPULSE_MAX);

    build_water_over_lava_scene(&real);

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = 20;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "water over lava scene, %dx%d: %lld us per step", REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);
    free(impulses);

    /* MEASURED 199,311 us per step, 2026-09-10 - this row's first real
     * device number, replacing the provisional ceiling it carried. Budget
     * is that x 0.9 rounded DOWN to 179,300. */
    perf_target("water over lava", per_step, 179300, 140173);
}

static void
test_the_gas_ignition_vessel_logs_the_blast_stress(void) {
    uint8_t* big = malloc((size_t)REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    impulse_t* impulses = malloc((size_t)GAS_IGNITION_VESSEL_IMPULSE_MAX * sizeof *impulses);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(impulses);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 71u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, GAS_IGNITION_VESSEL_IMPULSE_MAX);
    build_gas_ignition_vessel_scene(&real);

    int64_t totals[6] = {0};
    int64_t peak[6] = {0};
    int64_t peak_total = -1;
    int peak_impulses = 0;
    unsigned peak_blasts = 0;
    unsigned blasts_after_50 = 0;
    unsigned cap_hits_after_50 = 0;
    const two_core_scope_t core = two_core_scope_begin(true);
    for (int step = 1; step <= GAS_IGNITION_VESSEL_MEASURED_STEPS; step++) {
        const unsigned cap_before = real.impulse_cap_hits;
        sand_step(&real, 0, 1000, 0);
        const int64_t pass[6] = {real.pass_us.sweep_us, real.pass_us.liquid_us,    real.pass_us.float_us,
                                 real.pass_us.gas_us,   real.pass_us.reactions_us, real.pass_us.impulses_us};
        int64_t total = 0;
        for (int i = 0; i < 6; i++) {
            totals[i] += pass[i];
            total += pass[i];
        }
        if (total > peak_total) {
            memcpy(peak, pass, sizeof peak);
            peak_total = total;
            peak_impulses = real.impulse_count;
            peak_blasts = real.explosions_this_step;
        }
        if (step <= 50) {
            ESP_LOGI("device_tests",
                     "gas ignition vessel step=%d blasts=%u live=%d dropped=%u sweep=%lld liq=%lld "
                     "flt=%lld gas=%lld react=%lld imp=%lld us",
                     step, real.explosions_this_step, real.impulse_count, real.impulse_cap_hits - cap_before,
                     (long long)pass[0], (long long)pass[1], (long long)pass[2], (long long)pass[3], (long long)pass[4],
                     (long long)pass[5]);
        } else {
            blasts_after_50 += real.explosions_this_step;
            cap_hits_after_50 += real.impulse_cap_hits - cap_before;
        }
    }
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "gas ignition vessel scene, %dx%d: steps=%d, post50 mean blasts=%u live=%d dropped=%u",
             REAL_W, REAL_H, GAS_IGNITION_VESSEL_MEASURED_STEPS,
             blasts_after_50 / (GAS_IGNITION_VESSEL_MEASURED_STEPS - 50), real.impulse_count, cap_hits_after_50);
    log_pass_split("gas ignition vessel scene", GAS_IGNITION_VESSEL_MEASURED_STEPS, real.impulse_max, totals, peak,
                   peak_impulses, real.impulse_cap_hits);
    ESP_LOGI("device_tests", "gas ignition vessel scene: peak blasts=%u", peak_blasts);

    free(big);
    free(blocks);
    free(impulses);
}

/* The gunpowder basin scene (build_gunpowder_basin_scene(),
 * suite_sand_scenes.c), shared with the coverage test that proves the
 * chain-detonation really spans several bursts and reaches fuel
 * outside the vessel. */

/* NINETY STEPS, NO SETTLING - matching the coverage test exactly, so
 * this times the same run already proved to reach every path it
 * claims to. See GUNPOWDER_BASIN_MEASURED_STEPS's own comment
 * (suite_sand_scenes.c) for the timeline that window came from. */

/* MEASURED 31,399 us per step on device, 2026-09-06, first clean run of
 * this row. Budget is that x 0.9 = 28,259, rounded DOWN to 28,200 so the
 * target is never looser than the convention. */

/* SO THIS ROW FAILS BY DESIGN, like every other budget in this section:
 * a reduction target, not a regression guard. Re-peg only from a fresh
 * capture, never to make it green. */
static void
test_the_gunpowder_basin_scene_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc((size_t)REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    impulse_t* impulses = malloc((size_t)GUNPOWDER_BASIN_IMPULSE_MAX * sizeof *impulses);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(impulses);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 61u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_enable_impulses(&real, impulses, GUNPOWDER_BASIN_IMPULSE_MAX);

    build_gunpowder_basin_scene(&real);

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    const int steps = GUNPOWDER_BASIN_MEASURED_STEPS;
    int64_t pass_totals[6] = {0};
    int64_t pass_peak[6] = {0};
    int64_t peak_total = -1;
    int peak_impulses = 0;
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
        const int64_t pass[6] = {real.pass_us.sweep_us, real.pass_us.liquid_us,    real.pass_us.float_us,
                                 real.pass_us.gas_us,   real.pass_us.reactions_us, real.pass_us.impulses_us};
        int64_t total = 0;
        for (int j = 0; j < 6; j++) {
            pass_totals[j] += pass[j];
            total += pass[j];
        }
        if (total > peak_total) {
            memcpy(pass_peak, pass, sizeof pass_peak);
            peak_total = total;
            peak_impulses = real.impulse_count;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "gunpowder basin scene, %dx%d: %lld us per step", REAL_W, REAL_H, (long long)per_step);
    log_pass_split("gunpowder basin scene", steps, real.impulse_max, pass_totals, pass_peak, peak_impulses,
                   real.impulse_cap_hits);

    free(big);
    free(blocks);
    free(impulses);

    perf_target("gunpowder basin", per_step, 28200, 34645);
}

/* --- the interaction round's three scenes -------------------------------
 *
 * Picked from a 380-pairing arena rather than from the shape of the board.
 * Each builder (suite_sand_scenes.c) carries the measurement that earned it
 * a row, and the coverage test beside it proves the scene does that inside
 * the window timed here. */

/* Measured 83,173 / 12,114 / 46,265 us per step, perf-scoped, pegged at that
 * x 0.9 rounded DOWN - so all three ship RED, a reduction target rather than
 * a guard, as every row here was first set. The host ranked all three right
 * and priced none: 137x, 177x, 176x against the 179-214x its comparable rows
 * predicted. */
#define PLANT_RUIN_BUDGET_US    74800
#define FILLING_BASIN_BUDGET_US 10900
#define SNOWFALL_BUDGET_US      41600

/* 84,706 us a step, perf-scoped, pegged at that x 0.9 rounded down like the
 * three above - the third dearest scene in the suite, behind water over
 * lava and a packed screen of gas. */
#define PLANT_POUR_BUDGET_US    76200

/* 60 us, pegged the same way, and the number worth writing down: the same
 * board cost 28,362 before a landed plant stopped arming the reaction pass
 * (see may_have_faller/faller_may_move in sand.h). What is left is the
 * sweep's own block scan - knowingly red at 16x32, see that row. */
#define PLANT_IDLE_BUDGET_US    54

/* THE ONE ROW HERE WITH NO DEVICE CAPTURE BEHIND IT: another round held the
 * board. Ranked, not priced - 138 us on the host against the growing bed's
 * 420 for the same board, applied to that row's device figure, then the
 * file-wide x 0.9. Replace it with a capture rather than trusting it. */
#define MATURE_TREE_BUDGET_US   21600

/* A grown plant bed with acid eating down to its roots on one side of a wall
 * and lava burning its canopy on the other (build_plant_ruin_scene(), shared
 * with test_the_plant_ruin_scene_eats_roots_and_burns_a_canopy). The acid
 * leads the lava by PLANT_RUIN_ACID_LEAD_STEPS because the two do not peak
 * together - see that constant. */
static void
test_the_plant_ruin_scene_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_plant_ruin_scene(&real);
    for (int i = 0; i < PLANT_BED_SETTLE_STEPS; i++) {
        if (i == PLANT_BED_RAIN_A || i == PLANT_BED_RAIN_B) {
            plant_bed_rain(&real);
        }
        sand_step(&real, 0, 1000, 0);
    }
    for (int i = 0; i < PLANT_RUIN_ACID_LEAD_STEPS; i++) {
        if (i % PLANT_RUIN_ACID_EVERY == 0) {
            plant_ruin_acid_pour(&real);
        }
        sand_step(&real, 0, 1000, 0);
    }
    plant_ruin_lava_pour(&real);

    const int steps = PLANT_RUIN_MEASURED_STEPS;
    int64_t worst = 0;
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        if (i % PLANT_RUIN_ACID_EVERY == 0) {
            plant_ruin_acid_pour(&real);
        }
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, 0, 1000, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "plant ruin scene, %dx%d: %lld us per step, "
             "worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    /* THE INTERACTION IS THE FINDING: the same bed, grown the same way, is
     * 68,076 us a step while it is merely drinking rain and 83,173 once acid
     * and lava arrive - 22% for the pours alone. */
    perf_target("plant ruin", per_step, PLANT_RUIN_BUDGET_US, 71686);
}

/* Water running down a ramp into a pool (build_filling_basin_scene(), shared
 * with test_the_filling_basin_scene_runs_from_the_lip_to_the_pool) - the
 * companion to the free-falling slab above, on the same board and with a
 * comparable body of water, but settling rather than dropping into vacuum.
 * The slab row is deliberately left exactly as it was: PRs #174 and #175
 * quote its numbers, and redefining it would invalidate that history. */
static void
test_the_filling_basin_scene_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);

    build_filling_basin_scene(&real);
    for (int i = 0; i < FILLING_BASIN_SETTLE_STEPS; i++) {
        if (i % FILLING_BASIN_POUR_EVERY == 0) {
            filling_basin_pour(&real);
        }
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = FILLING_BASIN_MEASURED_STEPS;
    int64_t worst = 0;
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        if (i % FILLING_BASIN_POUR_EVERY == 0) {
            filling_basin_pour(&real);
        }
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, 0, 1000, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "filling basin scene, %dx%d: %lld us per step, "
             "worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    /* WHAT THE PAIR SAYS, and it is the reason this row exists: the slab row
     * above measured 12,060 us a step in the same capture, this one 16,077.
     * A third more for the same board of water, purely for settling rather
     * than dropping into vacuum - so the row the water work is tuned on is
     * the cheaper of the two cases by 33%. */
    perf_target("filling basin", per_step, FILLING_BASIN_BUDGET_US, 12579);
}

/* Snow falling onto a bank that has already crusted, over sand and dirt
 * (build_snowfall_scene(), shared with test_the_snowfall_scene_holds_a_
 * crusting_bank_and_a_live_fall). Forced crust - see the builder's own
 * declaration for why a scene left at the shipped rate holds no ice at all
 * inside any window this file times. */
static void
test_the_snowfall_scene_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 23u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_set_crust(&real, CRUST_ROLL_MAX);

    build_snowfall_scene(&real);
    for (int i = 0; i < SNOWFALL_SETTLE_STEPS; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = SNOWFALL_MEASURED_STEPS;
    int64_t worst = 0;
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        if (i % SNOWFALL_DRIFT_EVERY == 0) {
            snowfall_drift(&real);
        }
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, 0, 1000, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "snowfall scene, %dx%d: %lld us per step, "
             "worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    /* 63,371 us a step from a material that had no scene at all: about what
     * a growing plant bed costs, and dearer than a campfire. */
    perf_target("snowfall", per_step, SNOWFALL_BUDGET_US, 40799);
}

/* The plant brush poured onto damp earth (build_plant_pour_scene()), which no
 * other row reaches: every plant scene here grows a garden, and a grown tree
 * is anchored, so its support walk returns on the first neighbour.
 *
 * Timed from the first stamp rather than after a settle - a settled heap is
 * the plant bed row over again. */
static void
test_pouring_the_plant_brush_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_plant_pour_scene(&real);

    for (int i = 0; i < PLANT_POUR_SETTLE_STEPS; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = PLANT_POUR_MEASURED_STEPS;
    int64_t worst = 0;
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        plant_pour_stamp(&real, i);
        const int64_t t0 = esp_timer_get_time();
        sand_step(&real, 0, 1000, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "plant pour, %dx%d: %lld us per step, "
             "worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    perf_target("plant pour", per_step, PLANT_POUR_BUDGET_US, 78560);
}

/* The same heap once it has stopped: the state a poured garden spends almost
 * all of its life in, and the one no other row measures. Every plant here is
 * landed or anchored, so the reaction pass has nothing it can do and the
 * number is whatever it costs to find that out. */
static void
test_a_settled_plant_garden_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_dry_plant_heap_scene(&real);

    for (int i = 0; i < PLANT_POUR_MEASURED_STEPS; i++) {
        plant_pour_stamp(&real, i);
        sand_step(&real, 0, 1000, 0);
    }
    for (int i = 0; i < PLANT_IDLE_SETTLE_STEPS; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = 200;
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "settled plant garden, %dx%d: %lld us per step", REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    perf_target("settled plant garden", per_step, PLANT_IDLE_BUDGET_US, 132);
}

/* The maintainer's own case: a tree grown from seed on damp earth, with wood,
 * leaves and a root system, left until it has both stopped growing and drunk
 * the ground dry. Every other plant row here is chosen for something still
 * happening in it; this one is chosen for nothing happening, because that is
 * what a garden does for all but the first few hundred steps of its life. */
static void
test_a_finished_tree_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 11u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_plant_bed_scene(&real);

    for (int i = 0; i < MATURE_TREE_SETTLE_STEPS; i++) {
        sand_step(&real, 0, 1000, 0);
    }

    const int steps = 200;
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        sand_step(&real, 0, 1000, 0);
    }
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "finished tree, %dx%d: %lld us per step", REAL_W, REAL_H, (long long)per_step);

    free(big);
    free(blocks);

    perf_target("finished tree", per_step, MATURE_TREE_BUDGET_US, 25583);
}

/* Every row above holds the board portrait, and the block shape behind the
 * settled-block skip was swept against exactly those rows. The board is
 * played LANDSCAPE, down grid +X - geometry in
 * suite_sand_scenes.h. Measured 42,290 / 54,458 / 11,618 us, perf-scoped at
 * block 16x32, pegged at that x 0.9 rounded down like every row above, so
 * all three ship red as reduction targets. */
#define LANDSCAPE_WATER_BUDGET_US      38000
#define LANDSCAPE_DEEP_WATER_BUDGET_US 49000
#define LANDSCAPE_SAND_BUDGET_US       10400

static int64_t
landscape_scene_us_per_step(sand_t* real, bool water, int64_t* worst_out) {
    const two_core_scope_t core = two_core_scope_begin(true);
    for (int i = 0; i < LANDSCAPE_PRIME_STEPS; i++) {
        if (water) {
            landscape_water_pour(real, i);
        } else {
            landscape_sand_pour(real, i);
        }
        sand_step(real, LANDSCAPE_GX, 0, 0);
    }

    const int steps = LANDSCAPE_MEASURED_STEPS;
    int64_t worst = 0;
    const int64_t start = esp_timer_get_time();
    for (int i = 0; i < steps; i++) {
        if (water) {
            landscape_water_pour(real, LANDSCAPE_PRIME_STEPS + i);
        } else {
            landscape_sand_pour(real, LANDSCAPE_PRIME_STEPS + i);
        }
        const int64_t t0 = esp_timer_get_time();
        sand_step(real, LANDSCAPE_GX, 0, 0);
        const int64_t took = esp_timer_get_time() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    *worst_out = worst;
    const int64_t per_step = (esp_timer_get_time() - start) / steps;
    two_core_scope_end(core);
    return per_step;
}

/* Water poured into a settled sand bed, held the way the board is played
 * (build_landscape_bed_scene(), shared with
 * test_the_landscape_beds_sleep_against_the_landscape_floor). The dearest
 * of the three, and the pairing the palette puts first. */
static void
test_pouring_water_into_a_landscape_sand_bed_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 29u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    build_landscape_bed_scene(&real);

    int64_t worst = 0;
    const int64_t per_step = landscape_scene_us_per_step(&real, true, &worst);

    ESP_LOGI("device_tests",
             "landscape water onto a sand bed, %dx%d: %lld "
             "us per step, worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    perf_target("landscape water", per_step, LANDSCAPE_WATER_BUDGET_US, 28507);
}

/* The same pour onto a bed holding 65% of the board rather than 40%: a
 * shorter drop, far more settled mass for the skip to win or lose, and the
 * arena's other priced landscape depth. */
static void
test_pouring_water_into_a_deep_landscape_bed_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 29u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    build_landscape_deep_bed_scene(&real);

    int64_t worst = 0;
    const int64_t per_step = landscape_scene_us_per_step(&real, true, &worst);

    ESP_LOGI("device_tests",
             "landscape water onto a deep sand bed, %dx%d: "
             "%lld us per step, worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    perf_target("deep landscape water", per_step, LANDSCAPE_DEEP_WATER_BUDGET_US, 31424);
}

/* The liquid-free landscape row. Without it a geometry change that moved
 * the two rows above could not be told apart from one that moved the liquid
 * passes, since every other liquid-free scene in this file is portrait. */
static void
test_pouring_sand_onto_a_landscape_sand_bed_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 29u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    build_landscape_bed_scene(&real);

    int64_t worst = 0;
    const int64_t per_step = landscape_scene_us_per_step(&real, false, &worst);

    ESP_LOGI("device_tests",
             "landscape sand onto a sand bed, %dx%d: %lld us "
             "per step, worst single step %lld us",
             REAL_W, REAL_H, (long long)per_step, (long long)worst);

    free(big);
    free(blocks);

    perf_target("landscape sand", per_step, LANDSCAPE_SAND_BUDGET_US, 9318);
}

/* --- gfx_present() cost against real sand scenes ------------------------
 *
 * Every frame-budget test above times sand_step() alone, with no drawing
 * involved - nothing has measured what gfx_present() actually costs against
 * the dirty pattern a real sand scene leaves (see suite_gfx.c for
 * synthetic-mark numbers only). These tests close that gap: build a real
 * scene, step it, reproduce app_sand.c's own marking policy, then time
 * gfx_present() on the result. */

/* REAL_W*REAL_CELL_PX == GFX_WIDTH and REAL_H*REAL_CELL_PX == GFX_HEIGHT -
 * REAL_W/REAL_H are the grid size at cell=2, the finest ("ULTRA") quality
 * tier in app_sand.c's qualities[] table, which is what the pixel math in
 * mirror_app_sand_marking()'s gfx_mark_dirty() calls has to agree with. */
#define REAL_CELL_PX 2

/* A present's cost is mostly bus time, so a timed present must PIN the panel
 * clock rather than inherit whatever an earlier test left the link at. The
 * begin presents once so the link reopen lands outside the timed window. */
typedef struct {
    int before_hz;
} panel_clock_scope_t;

static panel_clock_scope_t
panel_clock_scope_begin(int hz) {
    const panel_clock_scope_t scope = {.before_hz = gfx_panel_clock_hz()};
    gfx_set_panel_clock_hz(hz);
    gfx_heal_restore_defaults();
    gfx_present();
    return scope;
}

static void
panel_clock_scope_end(panel_clock_scope_t scope) {
    gfx_set_panel_clock_hz(scope.before_hz);
    gfx_heal_restore_defaults();
}

/* REPRODUCING, NOT CALLING: draw_dirty_rows()/draw_one_row()/paint_row()
 * (app_sand.c) are static, inlined at their one call site - sharing a hot
 * per-call function across a translation-unit boundary previously cost a
 * measured 26% regression elsewhere.
 * Duplicates draw_dirty_rows()'s ~15-line policy instead (same row_runs
 * calls, same order, same dirty gate); paints no pixels, since
 * gfx_present()'s cost depends only on marked regions, never colour. */

static void
mirror_app_sand_marking(const uint8_t* cells, int w, int h, uint8_t* dirty_rows, uint16_t* row_x0, uint16_t* row_x1,
                        uint8_t* row_n) {
    for (int cy = 0; cy < h; cy++) {
        if (!dirty_rows[cy]) {
            continue;
        }
        dirty_rows[cy] = 0;

        const uint8_t* row = &cells[(size_t)cy * w];

        int run_x0[ROW_MAX_RUNS], run_x1[ROW_MAX_RUNS];
        const int n = row_runs_find(row, w, SAND_EMPTY, run_x0, run_x1);

        uint16_t cur_x0[ROW_MAX_RUNS], cur_x1[ROW_MAX_RUNS];
        int cur_n;
        if (n < 0) {
            int x0, x1;
            row_runs_span_fallback(row, w, SAND_EMPTY, &x0, &x1);
            cur_x0[0] = (uint16_t)x0;
            cur_x1[0] = (uint16_t)x1;
            cur_n = 1;
        } else {
            for (int i = 0; i < n; i++) {
                cur_x0[i] = (uint16_t)run_x0[i];
                cur_x1[i] = (uint16_t)run_x1[i];
            }
            cur_n = n;
        }

        uint16_t* rprev_x0 = &row_x0[cy * ROW_MAX_RUNS];
        uint16_t* rprev_x1 = &row_x1[cy * ROW_MAX_RUNS];
        const int rprev_n = row_n[cy];

        uint16_t send_x0[2 * ROW_MAX_RUNS], send_x1[2 * ROW_MAX_RUNS];
        const int send_n = row_runs_reconcile(cur_x0, cur_x1, cur_n, rprev_x0, rprev_x1, rprev_n, send_x0, send_x1);

        for (int i = 0; i < send_n; i++) {
            gfx_mark_dirty(send_x0[i] * REAL_CELL_PX, cy * REAL_CELL_PX, (send_x1[i] - send_x0[i]) * REAL_CELL_PX,
                           REAL_CELL_PX);
        }

        for (int i = 0; i < cur_n; i++) {
            rprev_x0[i] = cur_x0[i];
            rprev_x1[i] = cur_x1[i];
        }
        row_n[cy] = (uint8_t)cur_n;
    }
}

static void
seed_row_runs_full_width_for_gfx_test(uint16_t* row_x0, uint16_t* row_x1, uint8_t* row_n, int w, int h) {
    for (int i = 0; i < h; i++) {
        row_x0[i * ROW_MAX_RUNS] = 0;
        row_x1[i * ROW_MAX_RUNS] = (uint16_t)w;
        row_n[i] = 1;
    }
}

/* The settle frames are whole frames, so gfx's dirty state and row_runs'
 * "previous" reach what a running app sees before the timed window starts,
 * instead of measuring an inflated first frame.
 *
 * This is the PIPELINED price: queued bands drain together, so seven bands
 * in a real frame come to 18,147 us, not 7 x 3,405 = 23,835. suite_gfx.c's
 * ratio tests measure the un-pipelined price; the two do not convert by a
 * band count. */
static int64_t
run_present_against_scene(sand_t* s, const uint8_t* cells, int w, int h, uint8_t* dirty_rows, uint16_t* row_x0,
                          uint16_t* row_x1, uint8_t* row_n, int gx, int gy, int gz, int settle_steps,
                          int measured_steps, int* full_bands, int* gathered, int* partial_bands, int64_t* sim_us_out,
                          int64_t* mark_us_out, int64_t* present_us_out) {
    const two_core_scope_t core = two_core_scope_begin(true);
    const panel_clock_scope_t clock = panel_clock_scope_begin(GFX_PANEL_CLOCK_FAST_HZ);
    for (int i = 0; i < settle_steps; i++) {
        sand_step(s, gx, gy, gz);
        mirror_app_sand_marking(cells, w, h, dirty_rows, row_x0, row_x1, row_n);
        gfx_present();
    }

    gfx_reset_strip_send_counts();

    int64_t sim_us = 0, mark_us = 0, present_us = 0;
    for (int i = 0; i < measured_steps; i++) {
        const int64_t t0 = esp_timer_get_time();
        sand_step(s, gx, gy, gz);
        const int64_t t1 = esp_timer_get_time();
        mirror_app_sand_marking(cells, w, h, dirty_rows, row_x0, row_x1, row_n);
        const int64_t t2 = esp_timer_get_time();
        gfx_present();
        const int64_t t3 = esp_timer_get_time();

        sim_us += t1 - t0;
        mark_us += t2 - t1;
        present_us += t3 - t2;
    }

    gfx_get_strip_send_counts(full_bands, gathered, partial_bands);

    /* Per-step MEANS, same as the return value below - three phases of the
     * same measured window, so they share one averaging convention. */
    if (sim_us_out != NULL) {
        *sim_us_out = sim_us / measured_steps;
    }
    if (mark_us_out != NULL) {
        *mark_us_out = mark_us / measured_steps;
    }
    if (present_us_out != NULL) {
        *present_us_out = present_us / measured_steps;
    }
    panel_clock_scope_end(clock);
    two_core_scope_end(core);

    return present_us / measured_steps;
}

/* DENSE, CONTIGUOUS shape. Checkerboard exceeds ROW_MAX_RUNS (2).
 * row_runs_find() fails, row_runs_span_fallback() reports wide span.
 * gfx_present() handles. Scene shared with
 * test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene. Allocate
 * `big`, `dirty_rows`, `row_x0`, `row_x1`, `row_n` for sand_init(), tracking,
 * seeding. */
static void
build_falling_sand_present_scene(sand_t* real, uint8_t* big, uint8_t* dirty_rows, uint16_t* row_x0, uint16_t* row_x1,
                                 uint8_t* row_n) {
    sand_init(real, big, REAL_W, REAL_H, 99u);
    sand_track_dirty_rows(real, dirty_rows);
    seed_row_runs_full_width_for_gfx_test(row_x0, row_x1, row_n, REAL_W, REAL_H);

    for (int y = 0; y < REAL_H / 2; y++) {
        for (int x = 0; x < REAL_W; x++) {
            if (((x + y) & 1) == 0) {
                sand_set(real, x, y, SAND_FIRST_SHADE);
            }
        }
    }
}

static void
test_present_cost_against_a_falling_sand_scene(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* dirty_rows = malloc(REAL_H);
    uint16_t* row_x0 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t* row_x1 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t* row_n = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(dirty_rows);
    TEST_ASSERT_NOT_NULL(row_x0);
    TEST_ASSERT_NOT_NULL(row_x1);
    TEST_ASSERT_NOT_NULL(row_n);

    sand_t real;
    build_falling_sand_present_scene(&real, big, dirty_rows, row_x0, row_x1, row_n);

    int full_bands = 0, gathered = 0, partial_bands = 0;
    const int measured_steps = 20;
    const int64_t mean_us =
        run_present_against_scene(&real, big, REAL_W, REAL_H, dirty_rows, row_x0, row_x1, row_n, 0, 1, 0, 5,
                                  measured_steps, &full_bands, &gathered, &partial_bands, NULL, NULL, NULL);

    ESP_LOGI("device_tests",
             "present cost, falling sand checkerboard, "
             "%dx%d: mean %lld us/frame over %d frames "
             "(%d full-band, %d gathered, %d partial-band "
             "strip-sends)",
             REAL_W, REAL_H, (long long)mean_us, measured_steps, full_bands, gathered, partial_bands);

    free(big);
    free(dirty_rows);
    free(row_x0);
    free(row_x1);
    free(row_n);

    /* Present() is ~94% irreducible bus time (gfx.h;
     * test_full_present_cost_splits_into_bus_time_and_overhead) - the only
     * movable thing is HOW MANY strips get sent, shown by the strip-send
     * counts beside the timing. Target: measured 9961 * 0.97 -> 9650, NOT the
     * 0.9 sand_step() rows use: a 10% target on a 6%-reducible cost is
     * permanently unreachable, and 3% already asks for half the movable part.
     * Bound by different hardware (bus, not flash layout) - do not correct
     * this to 0.9. */
    perf_target("present: falling sand", mean_us, 9650, 6340);
}

/* Present tests run the sim outside their own timer. Neither measures the
 * frame SUM, needed before justification. PRINTS, no frame budget argued yet.
 * Missing the real pixel writes - a LOWER BOUND only. */
static void
test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* dirty_rows = malloc(REAL_H);
    uint16_t* row_x0 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t* row_x1 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t* row_n = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(dirty_rows);
    TEST_ASSERT_NOT_NULL(row_x0);
    TEST_ASSERT_NOT_NULL(row_x1);
    TEST_ASSERT_NOT_NULL(row_n);

    sand_t real;
    build_falling_sand_present_scene(&real, big, dirty_rows, row_x0, row_x1, row_n);

    int full_bands = 0, gathered = 0, partial_bands = 0;
    int64_t sim_us = 0, mark_us = 0, present_us = 0;
    const int measured_steps = 20;
    run_present_against_scene(&real, big, REAL_W, REAL_H, dirty_rows, row_x0, row_x1, row_n, 0, 1, 0, 5, measured_steps,
                              &full_bands, &gathered, &partial_bands, &sim_us, &mark_us, &present_us);

    free(big);
    free(dirty_rows);
    free(row_x0);
    free(row_x1);
    free(row_n);

    const int64_t total_us = sim_us + mark_us + present_us;
    const int present_pct = total_us > 0 ? (int)((present_us * 100) / total_us) : 0;

    ESP_LOGI("device_tests", "frame time, falling sand checkerboard: sim %lld us/frame", (long long)sim_us);
    ESP_LOGI("device_tests", "frame time, falling sand checkerboard: mark %lld us/frame", (long long)mark_us);
    ESP_LOGI("device_tests", "frame time, falling sand checkerboard: present %lld us/frame", (long long)present_us);
    ESP_LOGI("device_tests", "frame time, falling sand checkerboard: total %lld us/frame", (long long)total_us);
    ESP_LOGI("device_tests",
             "frame time, falling sand checkerboard: present is %d%% of "
             "the total",
             present_pct);
}

static void
test_present_cost_against_the_lava_stress_scene(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    uint8_t* dirty_rows = malloc(REAL_H);
    uint16_t* row_x0 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t* row_x1 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t* row_n = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(dirty_rows);
    TEST_ASSERT_NOT_NULL(row_x0);
    TEST_ASSERT_NOT_NULL(row_x1);
    TEST_ASSERT_NOT_NULL(row_n);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 37u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_track_dirty_rows(&real, dirty_rows);
    seed_row_runs_full_width_for_gfx_test(row_x0, row_x1, row_n, REAL_W, REAL_H);

    build_lava_stress_scene(&real);

    int full_bands = 0, gathered = 0, partial_bands = 0;
    int64_t sim_us = 0, mark_us = 0, present_us = 0;
    const int measured_steps = 20;
    const int64_t mean_us = run_present_against_scene(&real, big, REAL_W, REAL_H, dirty_rows, row_x0, row_x1, row_n, 0,
                                                      1000, 0, 30, measured_steps, &full_bands, &gathered,
                                                      &partial_bands, &sim_us, &mark_us, &present_us);

    /* THE WHOLE FRAME, not just the bus: every other
     * row here times sand_step() with no drawing, and the present rows
     * time the bus alone, so nothing measured the frame a user actually
     * sees. The helper already separates these three - this row was
     * discarding them. */
    ESP_LOGI("device_tests", "frame time, lava stress: sim %lld us/frame", (long long)sim_us);
    ESP_LOGI("device_tests", "frame time, lava stress: mark %lld us/frame", (long long)mark_us);
    ESP_LOGI("device_tests", "frame time, lava stress: present %lld us/frame", (long long)present_us);
    ESP_LOGI("device_tests", "frame time, lava stress: total %lld us/frame",
             (long long)(sim_us + mark_us + present_us));

    ESP_LOGI("device_tests",
             "present cost, lava stress scene, %dx%d: mean "
             "%lld us/frame over %d frames (%d full-band, "
             "%d gathered, %d partial-band strip-sends)",
             REAL_W, REAL_H, (long long)mean_us, measured_steps, full_bands, gathered, partial_bands);

    free(big);
    free(blocks);
    free(dirty_rows);
    free(row_x0);
    free(row_x1);
    free(row_n);

    perf_guard("present: lava stress", mean_us, 9797);
}

static void
test_present_cost_against_the_thermal_shock_scene(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    uint8_t* dirty_rows = malloc(REAL_H);
    uint16_t* row_x0 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t* row_x1 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t* row_n = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);
    TEST_ASSERT_NOT_NULL(dirty_rows);
    TEST_ASSERT_NOT_NULL(row_x0);
    TEST_ASSERT_NOT_NULL(row_x1);
    TEST_ASSERT_NOT_NULL(row_n);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    sand_set_scatter(&real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(&real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(&real, SAND_MOBILITY_PER_MATERIAL);
    sand_track_dirty_rows(&real, dirty_rows);
    seed_row_runs_full_width_for_gfx_test(row_x0, row_x1, row_n, REAL_W, REAL_H);

    build_thermal_shock_scene(&real);

    int full_bands = 0, gathered = 0, partial_bands = 0;
    int64_t sim_us = 0, mark_us = 0, present_us = 0;
    const int measured_steps = 10;
    const int64_t mean_us = run_present_against_scene(&real, big, REAL_W, REAL_H, dirty_rows, row_x0, row_x1, row_n, 0,
                                                      1000, 0, 0, measured_steps, &full_bands, &gathered,
                                                      &partial_bands, &sim_us, &mark_us, &present_us);

    /* THE WHOLE FRAME, not just the bus: every other
     * row here times sand_step() with no drawing, and the present rows
     * time the bus alone, so nothing measured the frame a user actually
     * sees. The helper already separates these three - this row was
     * discarding them. */
    ESP_LOGI("device_tests", "frame time, thermal shock: sim %lld us/frame", (long long)sim_us);
    ESP_LOGI("device_tests", "frame time, thermal shock: mark %lld us/frame", (long long)mark_us);
    ESP_LOGI("device_tests", "frame time, thermal shock: present %lld us/frame", (long long)present_us);
    ESP_LOGI("device_tests", "frame time, thermal shock: total %lld us/frame",
             (long long)(sim_us + mark_us + present_us));

    ESP_LOGI("device_tests",
             "present cost, thermal shock lattice, %dx%d: "
             "mean %lld us/frame over %d frames (%d "
             "full-band, %d gathered, %d partial-band "
             "strip-sends)",
             REAL_W, REAL_H, (long long)mean_us, measured_steps, full_bands, gathered, partial_bands);

    free(big);
    free(blocks);
    free(dirty_rows);
    free(row_x0);
    free(row_x1);
    free(row_n);

    /* 70/70 full strip-sends and zero gathered is correct, not a target:
     * this lattice dirties every strip every frame, so an oracle sends the
     * same 164,864 pixels. Watch pixels sent. A failure likely means the
     * scene dirties MORE pixels, not a slower present. */
    perf_guard("present: thermal shock", mean_us, 10655);
}

/* Present cost with column-precise dirty tracking, against the two scenes
 * a row-only policy costs the most on: gas rising clear of a sand pile, and
 * a pool levelling - both LANDSCAPE, where a row runs ALONG gravity, so a
 * changed cell's row can hold a long, unrelated run the old policy resent. */

/* Clamps row cy's dirty x-span to a one-cell margin, or to the whole row
 * when nothing narrower was recorded, and resets the span for next frame. */
static void
mirror_row_window(int w, int cy, uint16_t* dirty_x0, uint16_t* dirty_x1, int* wx0_out, int* wx1_out) {
    int wx0 = dirty_x0[cy];
    int wx1 = dirty_x1[cy];
    dirty_x0[cy] = (uint16_t)w;
    dirty_x1[cy] = 0;
    if (wx0 >= wx1) {
        wx0 = 0;
        wx1 = w;
    } else {
        wx0 = wx0 > 0 ? wx0 - 1 : 0;
        wx1 = wx1 < w ? wx1 + 1 : w;
    }
    *wx0_out = wx0;
    *wx1_out = wx1;
}

/* Fills cur_x0/cur_x1 with row's current empty-material runs, falling back
 * to one run spanning the row when it holds too many to list. Returns the
 * run count. */
static int
mirror_row_current_runs(const uint8_t* row, int w, uint16_t* cur_x0, uint16_t* cur_x1) {
    int run_x0[ROW_MAX_RUNS], run_x1[ROW_MAX_RUNS];
    const int n = row_runs_find(row, w, SAND_EMPTY, run_x0, run_x1);
    if (n < 0) {
        int x0, x1;
        row_runs_span_fallback(row, w, SAND_EMPTY, &x0, &x1);
        cur_x0[0] = (uint16_t)x0;
        cur_x1[0] = (uint16_t)x1;
        return 1;
    }
    for (int i = 0; i < n; i++) {
        cur_x0[i] = (uint16_t)run_x0[i];
        cur_x1[i] = (uint16_t)run_x1[i];
    }
    return n;
}

/* Marks each reconciled dirty run gfx-dirty, clamped to the row's window,
 * and tallies the pixels sent when the caller is counting them. */
static void
mirror_row_send_dirty(int cy, int wx0, int wx1, const uint16_t* send_x0, const uint16_t* send_x1, int send_n,
                      int64_t* pixels_sent_accum) {
    for (int i = 0; i < send_n; i++) {
        const int sx0 = send_x0[i] > wx0 ? send_x0[i] : wx0;
        const int sx1 = send_x1[i] < wx1 ? send_x1[i] : wx1;
        if (sx0 >= sx1) {
            continue;
        }
        gfx_mark_dirty(sx0 * REAL_CELL_PX, cy * REAL_CELL_PX, (sx1 - sx0) * REAL_CELL_PX, REAL_CELL_PX);
        if (pixels_sent_accum != NULL) {
            *pixels_sent_accum += (int64_t)(sx1 - sx0) * REAL_CELL_PX * REAL_CELL_PX;
        }
    }
}

static void
mirror_app_sand_marking_span(const uint8_t* cells, int w, int h, uint8_t* dirty_rows, uint16_t* dirty_x0,
                             uint16_t* dirty_x1, uint16_t* row_x0, uint16_t* row_x1, uint8_t* row_n,
                             int64_t* pixels_sent_accum) {
    for (int cy = 0; cy < h; cy++) {
        if (!dirty_rows[cy]) {
            continue;
        }
        dirty_rows[cy] = 0;

        int wx0, wx1;
        mirror_row_window(w, cy, dirty_x0, dirty_x1, &wx0, &wx1);

        const uint8_t* row = &cells[(size_t)cy * w];

        uint16_t cur_x0[ROW_MAX_RUNS], cur_x1[ROW_MAX_RUNS];
        const int cur_n = mirror_row_current_runs(row, w, cur_x0, cur_x1);

        uint16_t* rprev_x0 = &row_x0[cy * ROW_MAX_RUNS];
        uint16_t* rprev_x1 = &row_x1[cy * ROW_MAX_RUNS];
        const int rprev_n = row_n[cy];

        uint16_t send_x0[2 * ROW_MAX_RUNS], send_x1[2 * ROW_MAX_RUNS];
        const int send_n = row_runs_reconcile(cur_x0, cur_x1, cur_n, rprev_x0, rprev_x1, rprev_n, send_x0, send_x1);

        mirror_row_send_dirty(cy, wx0, wx1, send_x0, send_x1, send_n, pixels_sent_accum);

        for (int i = 0; i < cur_n; i++) {
            rprev_x0[i] = cur_x0[i];
            rprev_x1[i] = cur_x1[i];
        }
        row_n[cy] = (uint8_t)cur_n;
    }
}

/* Same shape as run_present_against_scene() above, `dirty_x0`/`dirty_x1`
 * added and routed through sand_track_dirty_cols() so the sim itself
 * starts recording a span, not just which rows changed. */
static int64_t
run_present_against_scene_span(sand_t* s, const uint8_t* cells, int w, int h, uint8_t* dirty_rows, uint16_t* dirty_x0,
                               uint16_t* dirty_x1, uint16_t* row_x0, uint16_t* row_x1, uint8_t* row_n, int gx, int gy,
                               int gz, int settle_steps, int measured_steps, int* full_bands, int* gathered,
                               int* partial_bands, int64_t* pixels_sent_out) {
    sand_track_dirty_cols(s, dirty_x0, dirty_x1);

    const two_core_scope_t core = two_core_scope_begin(true);
    const panel_clock_scope_t clock = panel_clock_scope_begin(GFX_PANEL_CLOCK_FAST_HZ);
    for (int i = 0; i < settle_steps; i++) {
        sand_step(s, gx, gy, gz);
        mirror_app_sand_marking_span(cells, w, h, dirty_rows, dirty_x0, dirty_x1, row_x0, row_x1, row_n, NULL);
        gfx_present();
    }

    gfx_reset_strip_send_counts();

    int64_t present_us = 0;
    int64_t pixels_sent = 0;
    for (int i = 0; i < measured_steps; i++) {
        sand_step(s, gx, gy, gz);
        mirror_app_sand_marking_span(cells, w, h, dirty_rows, dirty_x0, dirty_x1, row_x0, row_x1, row_n, &pixels_sent);
        const int64_t t0 = esp_timer_get_time();
        gfx_present();
        present_us += esp_timer_get_time() - t0;
    }

    gfx_get_strip_send_counts(full_bands, gathered, partial_bands);

    if (pixels_sent_out != NULL) {
        *pixels_sent_out = pixels_sent / measured_steps;
    }
    panel_clock_scope_end(clock);
    two_core_scope_end(core);
    return present_us / measured_steps;
}

/* The same 65%-deep settled pile the deep landscape bed perf row pours
 * onto - real repose slopes, not a drawn block. Gas goes in near the
 * ceiling, clear of the pile, rising further AWAY from the sand it shares
 * a row with. */
static void
build_landscape_gas_over_sand_pile_scene(sand_t* real, uint8_t* big, uint8_t* blocks) {
    sand_init(real, big, REAL_W, REAL_H, 53u);
    sand_enable_sleeping(real, blocks);
    sand_set_scatter(real, SAND_SCATTER_PER_MATERIAL);
    sand_set_decay(real, SAND_DECAY_PER_MATERIAL);
    sand_set_mobility(real, SAND_MOBILITY_PER_MATERIAL);
    build_landscape_deep_bed_scene(real);

    for (int y = REAL_H / 3; y < (REAL_H * 2) / 3; y++) {
        sand_set(real, LANDSCAPE_POUR_RADIUS, y, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
    }
}

#define POOL_UNEVEN_DEEP_X1    ((REAL_W * 6) / 10)
#define POOL_UNEVEN_SHALLOW_X1 ((REAL_W * 2) / 10)

/* A wedge, not a flat slab: half the rows filled deep, half shallow, along
 * gravity (+X). Levelling this needs cross-flow, which moves mass BETWEEN
 * rows once gravity runs along one - the shape the bug report's "pool
 * levelling" case names. */
static void
build_landscape_levelling_pool_scene(sand_t* real, uint8_t* big, uint8_t* blocks) {
    sand_init(real, big, REAL_W, REAL_H, 59u);
    sand_enable_sleeping(real, blocks);

    for (int y = 0; y < REAL_H; y++) {
        const int x1 = (y < REAL_H / 2) ? POOL_UNEVEN_DEEP_X1 : POOL_UNEVEN_SHALLOW_X1;
        for (int x = 0; x < x1; x++) {
            sand_set(real, x, y, CELL_MAKE(MAT_WATER, MASS_MAX));
        }
    }
}

/* Runs `build` under both the old row-only mirror and the new column-span
 * one, back to back on two freshly built copies of the same scene, so the
 * before/after numbers this prints come from one run rather than two
 * captures that could drift apart. */
static void
report_span_vs_row_present_cost(const char* scene_name, void (*build)(sand_t*, uint8_t*, uint8_t*), int gx, int gy) {
    uint8_t* row_big = malloc(REAL_W * REAL_H);
    uint8_t* row_blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    uint8_t* row_dirty = malloc(REAL_H);
    uint16_t* row_x0 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t* row_x1 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t* row_n = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(row_big);
    TEST_ASSERT_NOT_NULL(row_blocks);
    TEST_ASSERT_NOT_NULL(row_dirty);
    TEST_ASSERT_NOT_NULL(row_x0);
    TEST_ASSERT_NOT_NULL(row_x1);
    TEST_ASSERT_NOT_NULL(row_n);

    sand_t row_sim;
    build(&row_sim, row_big, row_blocks);
    sand_track_dirty_rows(&row_sim, row_dirty);
    seed_row_runs_full_width_for_gfx_test(row_x0, row_x1, row_n, REAL_W, REAL_H);

    int row_full = 0, row_gathered = 0, row_partial = 0;
    const int measured_steps = 20;
    const int64_t row_us =
        run_present_against_scene(&row_sim, row_big, REAL_W, REAL_H, row_dirty, row_x0, row_x1, row_n, gx, gy, 0, 20,
                                  measured_steps, &row_full, &row_gathered, &row_partial, NULL, NULL, NULL);

    free(row_big);
    free(row_blocks);
    free(row_dirty);
    free(row_x0);
    free(row_x1);
    free(row_n);

    uint8_t* span_big = malloc(REAL_W * REAL_H);
    uint8_t* span_blocks = malloc(REAL_BLOCK_COLS * REAL_BLOCK_ROWS);
    uint8_t* span_dirty = malloc(REAL_H);
    uint16_t* span_dirty_x0 = malloc(REAL_H * sizeof(uint16_t));
    uint16_t* span_dirty_x1 = malloc(REAL_H * sizeof(uint16_t));
    uint16_t* span_x0 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint16_t* span_x1 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    uint8_t* span_n = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(span_big);
    TEST_ASSERT_NOT_NULL(span_blocks);
    TEST_ASSERT_NOT_NULL(span_dirty);
    TEST_ASSERT_NOT_NULL(span_dirty_x0);
    TEST_ASSERT_NOT_NULL(span_dirty_x1);
    TEST_ASSERT_NOT_NULL(span_x0);
    TEST_ASSERT_NOT_NULL(span_x1);
    TEST_ASSERT_NOT_NULL(span_n);

    sand_t span_sim;
    build(&span_sim, span_big, span_blocks);
    sand_track_dirty_rows(&span_sim, span_dirty);
    seed_row_runs_full_width_for_gfx_test(span_x0, span_x1, span_n, REAL_W, REAL_H);

    int span_full = 0, span_gathered = 0, span_partial = 0;
    int64_t pixels_sent = 0;
    const int64_t span_us = run_present_against_scene_span(
        &span_sim, span_big, REAL_W, REAL_H, span_dirty, span_dirty_x0, span_dirty_x1, span_x0, span_x1, span_n, gx, gy,
        0, 20, measured_steps, &span_full, &span_gathered, &span_partial, &pixels_sent);

    free(span_big);
    free(span_blocks);
    free(span_dirty);
    free(span_dirty_x0);
    free(span_dirty_x1);
    free(span_x0);
    free(span_x1);
    free(span_n);

    ESP_LOGI("device_tests",
             "present cost, %s, %dx%d: ROW-only %lld us/frame (%d full, %d "
             "gathered, %d partial) vs COLUMN-span %lld us/frame (%d full, "
             "%d gathered, %d partial, %lld px/frame)",
             scene_name, REAL_W, REAL_H, (long long)row_us, row_full, row_gathered, row_partial, (long long)span_us,
             span_full, span_gathered, span_partial, (long long)pixels_sent);
}

static void
test_present_cost_against_a_landscape_gas_over_sand_pile(void) {
    report_span_vs_row_present_cost("landscape gas over a sand pile", build_landscape_gas_over_sand_pile_scene,
                                    LANDSCAPE_GX, 0);
}

static void
test_present_cost_against_a_landscape_levelling_pool(void) {
    report_span_vs_row_present_cost("landscape levelling pool", build_landscape_levelling_pool_scene, LANDSCAPE_GX, 0);
}
#endif /* DEVICE_BUILD */

#define BUBBLE_W 41
#define BUBBLE_H 30

/* Fills [0,w) x [pool_top,h) of g with a full-mass acid pool. */
static void
fill_acid_pool(sand_t* g, int w, int pool_top, int h) {
    for (int y = pool_top; y < h; y++) {
        for (int x = 0; x < w; x++) {
            sand_set(g, x, y, CELL_MAKE(MAT_ACID, MASS_MAX));
        }
    }
}

/* Counts, in the surface band y < pool_top, how many currently-acid cells
 * lie left vs right of x = mid. */
static void
count_bubbles_by_side(const sand_t* g, int w, int pool_top, int mid, int* left_pops, int* right_pops) {
    for (int y = 0; y < pool_top; y++) {
        for (int x = 0; x < w; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) == MAT_ACID) {
                if (x < mid) {
                    (*left_pops)++;
                } else if (x > mid) {
                    (*right_pops)++;
                }
            }
        }
    }
}

/* True once any cell in [0,w) x [0,band_h) currently holds acid. */
static bool
band_has_acid(const sand_t* g, int w, int band_h) {
    for (int y = 0; y < band_h; y++) {
        for (int x = 0; x < w; x++) {
            if (CELL_MATERIAL(sand_at(g, x, y)) == MAT_ACID) {
                return true;
            }
        }
    }
    return false;
}

/* acid_bubble() (sand_reactions.c) replaced splash_displace()'s old
 * "landed hard on already-occupied liquid" trigger for acid: a real-scene
 * reproduction found landing events concentrating against whichever wall
 * cross-flow reached first - an emergent, self-reinforcing bias with no
 * single buggy line behind it. acid_bubble()'s flat, independent, per-cell
 * roll has no such feedback loop. */
static void
test_acid_bubbles_do_not_favour_one_wall(void) {
    /* NO POUR NEEDED: acid_bubble() checks every acid cell the REACTIONS
     * pass visits, every step, for open space against gravity, so a flat,
     * static pool's own exposed surface alone keeps it rolling. */
    enum { POOL_TOP = 15 };

    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. */
    uint8_t* bubble_cells = malloc((size_t)BUBBLE_W * BUBBLE_H);
    impulse_t* bubble_buf = malloc(512 * sizeof *bubble_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(bubble_cells, "acid-bubble pool grid must fit in what the framebuffer leaves");
    TEST_ASSERT_NOT_NULL_MESSAGE(bubble_buf, "acid-bubble impulse queue must fit in what the framebuffer leaves");
    sand_init(&fx.bubble_sim, bubble_cells, BUBBLE_W, BUBBLE_H, 3u);
    sand_enable_impulses(&fx.bubble_sim, bubble_buf, 512);

    /* NOT sleeping-enabled here, unlike test_acid_bubbles_still_bubble_
     * once_the_block_is_asleep below: this test's job is the SPATIAL claim
     * (no wall favoured), that test's is SLEEPING. FULL GRID WIDTH, NO
     * MARGIN: a pool with room to spread lowers its own surface over
     * hundreds of steps (same mass, wider footprint), so a fixed "above
     * POOL_TOP" check would end up looking above where the surface once
     * sat, not where it is. */
    fill_acid_pool(&fx.bubble_sim, BUBBLE_W, POOL_TOP, BUBBLE_H);

    int left_pops = 0, right_pops = 0;
    const int mid = BUBBLE_W / 2;
    for (int i = 0; i < 300; i++) {
        sand_step(&fx.bubble_sim, 0, 1000, 0);
        count_bubbles_by_side(&fx.bubble_sim, BUBBLE_W, POOL_TOP, mid, &left_pops, &right_pops);
    }

    /* Freed BEFORE the assertions: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. */
    free(bubble_cells);
    free(bubble_buf);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, left_pops + right_pops,
                                     "acid_bubble() must actually pop grains above an exposed surface "
                                     "over time - none appeared at all");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, left_pops,
                                     "bubbles must reach the left half of the surface, not just the "
                                     "right - see this test's own top comment for the exact regression "
                                     "this guards against");
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, right_pops,
                                     "bubbles must reach the right half of the surface, not just the "
                                     "left - see this test's own top comment for the exact regression "
                                     "this guards against");
}

#define SLEEPY_BLOCK_COLS ((BUBBLE_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W)
#define SLEEPY_BLOCK_ROWS ((BUBBLE_H + SAND_BLOCK_H - 1) / SAND_BLOCK_H)
static uint8_t sleepy_bubble_blocks[SLEEPY_BLOCK_COLS * SLEEPY_BLOCK_ROWS];

/* Bubbling must survive block sleeping. step_one_row() (sand.c) skips a
 * settled block entirely, so anything hung off move_liquid_grain() stops
 * the moment a puddle goes calm - exactly when bubbling is meant to prove
 * it is still alive. acid_bubble() lives in the reactions pass, which is
 * not block-gated, for the same reason dissolving and cooling are not.
 * This pool is settled to a confirmed sand_block_settled() before a single
 * bubble is allowed to count. */
static void
test_acid_bubbles_still_fire_once_the_block_is_asleep(void) {
    enum { POOL_TOP = 15 };

    /* HEAP, not static file scope - see drop_impulse_buf's own comment
     * above for why this file's static test fixtures cannot share the
     * framebuffer's memory budget. sleepy_bubble_blocks stays static -
     * it is a tiny sleep-state bitmap, not one of the buffers that
     * starved the device heap. */
    uint8_t* sleepy_bubble_cells = malloc((size_t)BUBBLE_W * BUBBLE_H);
    impulse_t* sleepy_bubble_buf = malloc(512 * sizeof *sleepy_bubble_buf);
    TEST_ASSERT_NOT_NULL_MESSAGE(sleepy_bubble_cells, "sleepy acid-bubble pool grid must fit in what the framebuffer "
                                                      "leaves");
    TEST_ASSERT_NOT_NULL_MESSAGE(sleepy_bubble_buf, "sleepy acid-bubble impulse queue must fit in what the "
                                                    "framebuffer leaves");
    sand_init(&fx.sleepy_bubble_sim, sleepy_bubble_cells, BUBBLE_W, BUBBLE_H, 3u);
    sand_enable_sleeping(&fx.sleepy_bubble_sim, sleepy_bubble_blocks);
    sand_enable_impulses(&fx.sleepy_bubble_sim, sleepy_bubble_buf, 512);

    fill_acid_pool(&fx.sleepy_bubble_sim, BUBBLE_W, POOL_TOP, BUBBLE_H);
    /* GLASS LID to ensure "must fall asleep" setup check is independent of
     * SAND_ACID_BUBBLE_CHANCE. acid_bubble() rolls only for open space
     * against gravity. Lid prevents acid cell exposure, ensuring pool settles
     * by physics alone. Removed once asleep confirmed. */
    for (int x = 0; x < BUBBLE_W; x++) {
        sand_set(&fx.sleepy_bubble_sim, x, POOL_TOP - 1, GLASS);
    }

    bool asleep = false;
    for (int i = 0; i < 40 && !asleep; i++) {
        sand_step(&fx.sleepy_bubble_sim, 0, 1000, 0);
        asleep = count_awake_blocks(&fx.sleepy_bubble_sim) == 0;
    }
    TEST_ASSERT_TRUE_MESSAGE(asleep, "setup: the pool must actually fall asleep within 40 quiet steps, "
                                     "or this test is not exercising the sleeping path it exists to "
                                     "check at all");

    for (int x = 0; x < BUBBLE_W; x++) {
        sand_erase(&fx.sleepy_bubble_sim, x, POOL_TOP - 1, 0);
    }

    int pops = 0;
    for (int i = 0; i < 300 && pops == 0; i++) {
        sand_step(&fx.sleepy_bubble_sim, 0, 1000, 0);
        if (band_has_acid(&fx.sleepy_bubble_sim, BUBBLE_W, POOL_TOP)) {
            pops = 1;
        }
    }

    /* Freed BEFORE the assertion: Unity longjmps out of a failure, so a
     * free() after one never runs - see drop_impulse_buf's own comment
     * above. All reads of sleepy_bubble_cells/sleepy_bubble_buf are done
     * by this point. */
    free(sleepy_bubble_cells);
    free(sleepy_bubble_buf);

    TEST_ASSERT_GREATER_THAN_MESSAGE(0, pops,
                                     "acid_bubble() must keep firing even after its block has gone to "
                                     "sleep - see this test's own top comment for the exact bug this "
                                     "guards against (a real, calm puddle on device that never bubbled "
                                     "at all)");
}

/* --- water slope: reported gravity-flip drop over a covered slope -------- */

#ifdef DEVICE_BUILD
static int
water_slope_liquid_near_blocks(const sand_t* s) {
    int n = 0;
    for (int by = 0; by < s->block_rows; by++) {
        for (int bx = 0; bx < s->block_cols; bx++) {
            if ((s->block_state[(size_t)by * (size_t)s->block_cols + (size_t)bx] & BLOCK_LIQUID_NEAR) != 0) {
                n++;
            }
        }
    }
    return n;
}

static long
water_slope_water_mass(const sand_t* s) {
    long total = 0;
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            const cell_t c = sand_at(s, x, y);
            if (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == MAT_WATER) {
                total += CELL_VARIANT(c);
            }
        }
    }
    return total;
}

/* One line per sand_step(), every pass split plus the counts task the
 * mechanism to a pass rather than only a total. Counters are cumulative
 * (sand_priv.h) - only the delta since the previous step is meaningful
 * here. */
static void
water_slope_log_step(const char* phase, int step_index, const sand_t* s, unsigned dispatched_delta,
                     unsigned moves_delta, unsigned probes_delta, unsigned sweep_moves_delta) {
    ESP_LOGI("device_tests",
             "%-8s %3d tot=%5d sweep=%4d liq=%4d flt=%3d gas=%3d react=%4d imp=%3d dispatch=%5u xmoves=%4u "
             "xprobes=%5u smoves=%4u soak=%d awake=%3d liqnear=%3d aborts=%3u",
             phase, step_index,
             (int)(s->pass_us.sweep_us + s->pass_us.liquid_us + s->pass_us.float_us + s->pass_us.gas_us
                   + s->pass_us.reactions_us + s->pass_us.impulses_us),
             (int)s->pass_us.sweep_us, (int)s->pass_us.liquid_us, (int)s->pass_us.float_us, (int)s->pass_us.gas_us,
             (int)s->pass_us.reactions_us, (int)s->pass_us.impulses_us, dispatched_delta, moves_delta, probes_delta,
             sweep_moves_delta, (int)sand_reactions_last_was_soak_only, count_awake_blocks(s),
             water_slope_liquid_near_blocks(s), s->split_lane_aborts);
}

static void
water_slope_step_and_log(sand_t* s, int gx, int gy, const char* phase, int step_index) {
    const unsigned d0 = sand_reactions_cells_dispatched;
    const unsigned m0 = sand_liquid_moves;
    const unsigned p0 = sand_liquid_crossflow_probes;
    const unsigned sw0 = sand_liquid_sweep_moves;

    sand_step(s, gx, gy, 0);

    water_slope_log_step(phase, step_index, s, sand_reactions_cells_dispatched - d0, sand_liquid_moves - m0,
                         sand_liquid_crossflow_probes - p0, sand_liquid_sweep_moves - sw0);
}

static void
log_pass_split(const char* name, int steps, int impulse_max, const int64_t totals[6], const int64_t peak[6],
               int peak_impulses, unsigned cap_hits) {
    ESP_LOGI("device_tests",
             "%s: mean tot=%lld sweep=%lld liq=%lld flt=%lld gas=%lld react=%lld imp=%lld us, peak tot=%lld "
             "sweep=%lld liq=%lld flt=%lld gas=%lld react=%lld imp=%lld us, impulse_peak=%d/%d cap=%s",
             name, (long long)((totals[0] + totals[1] + totals[2] + totals[3] + totals[4] + totals[5]) / steps),
             (long long)(totals[0] / steps), (long long)(totals[1] / steps), (long long)(totals[2] / steps),
             (long long)(totals[3] / steps), (long long)(totals[4] / steps), (long long)(totals[5] / steps),
             (long long)(peak[0] + peak[1] + peak[2] + peak[3] + peak[4] + peak[5]), (long long)peak[0],
             (long long)peak[1], (long long)peak[2], (long long)peak[3], (long long)peak[4], (long long)peak[5],
             peak_impulses, impulse_max, impulse_max == 0 ? "disabled" : (cap_hits != 0 ? "hit" : "not-hit"));
}

/* THE PRIMARY REPRO: no tilt, no diagonal, just a plain landscape pile
 * fully submerged with headroom, settled undisturbed. Logs a checkpoint
 * every 100 steps through the long settle to show whether awake blocks are
 * genuinely stuck or slowly converging, and asserts the pile actually
 * reaches full sleep within its settle budget - see
 * build_submerged_pile_scene()'s own comment for the measured convergence
 * time this budget is set from. */
static void
test_submerged_pile_settles_and_logs_the_pass_split(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc((size_t)REAL_BLOCK_COLS * (size_t)REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 29u);
    sand_enable_sleeping(&real, blocks);
    sand_set_soak(&real, SAND_SOAK_PER_MATERIAL);
    build_landscape_bed_scene(&real);

    for (int i = 0; i < SUBMERGED_PILE_POUR_STEPS; i++) {
        landscape_water_pour(&real, i);
        sand_step(&real, LANDSCAPE_GX, 0, 0);
    }
    const long mass_before = water_slope_water_mass(&real);

    int settled_at = -1;
    for (int i = 0; i < SUBMERGED_PILE_FULL_SETTLE_STEPS; i++) {
        const unsigned d0 = sand_reactions_cells_dispatched;
        const unsigned m0 = sand_liquid_moves;
        const unsigned p0 = sand_liquid_crossflow_probes;
        const unsigned sw0 = sand_liquid_sweep_moves;
        sand_step(&real, LANDSCAPE_GX, 0, 0);
        const int awake = count_awake_blocks(&real);
        if (i % 100 == 0 || i == SUBMERGED_PILE_FULL_SETTLE_STEPS - 1) {
            water_slope_log_step("settle", i, &real, sand_reactions_cells_dispatched - d0, sand_liquid_moves - m0,
                                 sand_liquid_crossflow_probes - p0, sand_liquid_sweep_moves - sw0);
        }
        if (awake == 0 && settled_at < 0) {
            settled_at = i;
        }
    }
    const long mass_after = water_slope_water_mass(&real);
    const int awake_at_end = count_awake_blocks(&real);

    free(big);
    free(blocks);

    ESP_LOGI("device_tests", "submerged pile: settled_at step %d of %d, awake_at_end=%d", settled_at,
             SUBMERGED_PILE_FULL_SETTLE_STEPS, awake_at_end);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE((int)mass_after, (int)mass_before,
                                             "settling a submerged pile must never create water - soaking may "
                                             "only ever spend it");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, awake_at_end,
                                  "a plain submerged pile with no further disturbance must reach full "
                                  "sleep given enough settle steps");
}

/* Task 1a: water poured continuously at the slope's high corner until it
 * covers the slope and runs down. Logs the pass split averaged over the
 * pour, then asserts only that real work happened - this scene exists to
 * characterise a cost, not to gate one yet. */
static void
test_water_slope_pouring_water_logs_the_pass_split(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc((size_t)REAL_BLOCK_COLS * (size_t)REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    build_water_slope_scene(&real);

    int64_t liquid_total = 0, reactions_total = 0, sweep_total = 0;
    const int steps = WATER_SLOPE_COVER_STEPS;
    for (int i = 0; i < steps; i++) {
        water_slope_water_pour(&real, i);
        sand_step(&real, LANDSCAPE_GX, 0, 0);
        sweep_total += real.pass_us.sweep_us;
        liquid_total += real.pass_us.liquid_us;
        reactions_total += real.pass_us.reactions_us;
    }
    const long mass = water_slope_water_mass(&real);
    const int liq_near = water_slope_liquid_near_blocks(&real);

    free(big);
    free(blocks);

    ESP_LOGI("device_tests", "water slope pour, %d steps: mean sweep=%d liq=%d react=%d us, water_mass=%ld liqnear=%d",
             steps, (int)(sweep_total / steps), (int)(liquid_total / steps), (int)(reactions_total / steps), mass,
             liq_near);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)mass, "the pour must actually place water on the board");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, liq_near,
                                         "the pour must reach BLOCK_LIQUID_NEAR blocks, or the "
                                         "reactions soak-only skip has nothing to walk");
}

/* Task 1c: the three controls beside the covered slope, one line each - a
 * dry slope (no liquid pass at all), water over a flat pile (same pour
 * mechanism, no diagonal), and water over stone (the same diagonal, nothing
 * wettable). Compares them against the covered slope on the counters that
 * matter: reactions dispatch and cross-flow moves/probes. */
static void
test_water_slope_controls_log_the_pass_split(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc((size_t)REAL_BLOCK_COLS * (size_t)REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    long masses[4] = {0};
    const char* names[4] = {"dry", "slope", "flat", "stone"};

    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    build_water_slope_scene(&real);
    for (int i = 0; i < 60; i++) {
        sand_step(&real, LANDSCAPE_GX, 0, 0);
    }
    water_slope_step_and_log(&real, LANDSCAPE_GX, 0, names[0], 0);
    masses[0] = water_slope_water_mass(&real);

    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    build_water_slope_covered_scene(&real);
    water_slope_step_and_log(&real, LANDSCAPE_GX, 0, names[1], 0);
    masses[1] = water_slope_water_mass(&real);

    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    build_water_slope_flat_covered_scene(&real);
    water_slope_step_and_log(&real, LANDSCAPE_GX, 0, names[2], 0);
    masses[2] = water_slope_water_mass(&real);

    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    build_water_slope_stone_covered_scene(&real);
    water_slope_step_and_log(&real, LANDSCAPE_GX, 0, names[3], 0);
    masses[3] = water_slope_water_mass(&real);

    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)masses[0], "the dry control must hold no water at all");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)masses[1], "the covered slope must hold water");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)masses[2], "the flat-pile control must hold water");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)masses[3], "the stone control must hold water");
}

/* Task 1b, and the maintainer's own refinement: settle the covered slope in
 * landscape, tilt right into portrait, hold, tilt back. One line per step
 * through the whole sequence - the flip itself, not only its settled ends,
 * is what the report says is worst. */
static void
test_water_slope_gravity_flip_logs_a_per_step_table(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc((size_t)REAL_BLOCK_COLS * (size_t)REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    build_water_slope_covered_scene(&real);

    const long mass_before = water_slope_water_mass(&real);

    for (int i = 0; i < WATER_SLOPE_FLIP_SETTLE_STEPS; i++) {
        water_slope_step_and_log(&real, LANDSCAPE_GX, 0, "settle", i);
    }
    for (int i = 1; i <= WATER_SLOPE_FLIP_TURN_STEPS; i++) {
        const int gx = LANDSCAPE_GX - (LANDSCAPE_GX * i) / WATER_SLOPE_FLIP_TURN_STEPS;
        const int gy = (WATER_SLOPE_PORTRAIT_GY * i) / WATER_SLOPE_FLIP_TURN_STEPS;
        water_slope_step_and_log(&real, gx, gy, "to_port", i);
    }
    for (int i = 0; i < WATER_SLOPE_FLIP_HOLD_STEPS; i++) {
        water_slope_step_and_log(&real, WATER_SLOPE_PORTRAIT_GX, WATER_SLOPE_PORTRAIT_GY, "hold", i);
    }
    for (int i = 1; i <= WATER_SLOPE_FLIP_TURN_STEPS; i++) {
        const int gx = (LANDSCAPE_GX * i) / WATER_SLOPE_FLIP_TURN_STEPS;
        const int gy = WATER_SLOPE_PORTRAIT_GY - (WATER_SLOPE_PORTRAIT_GY * i) / WATER_SLOPE_FLIP_TURN_STEPS;
        water_slope_step_and_log(&real, gx, gy, "to_land", i);
    }
    for (int i = 0; i < WATER_SLOPE_FLIP_HOLD_STEPS; i++) {
        water_slope_step_and_log(&real, LANDSCAPE_GX, 0, "recover", i);
    }

    const long mass_after = water_slope_water_mass(&real);

    free(big);
    free(blocks);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE((int)mass_after, (int)mass_before,
                                             "a tilt right into portrait and back must never create water - "
                                             "soaking may only ever spend it");
}

/* The mid-task correction: a scene seeded directly from a device screenshot
 * of the reported drop, gravity swept between the two tilt vectors the same
 * capture pair recorded - a partly diagonal change, not an axis-aligned
 * flip. */
static void
test_water_slope_captured_scene_diagonal_flip_logs_a_per_step_table(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    uint8_t* blocks = malloc((size_t)REAL_BLOCK_COLS * (size_t)REAL_BLOCK_ROWS);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(blocks);

    sand_t real;
    sand_init(&real, big, REAL_W, REAL_H, 41u);
    sand_enable_sleeping(&real, blocks);
    build_captured_water_slope_scene(&real);

    const long mass_before = water_slope_water_mass(&real);

    for (int i = 1; i <= WATER_SLOPE_CAPTURED_SWEEP_STEPS; i++) {
        const int gx =
            WATER_SLOPE_CAPTURED_TILT1_GX
            + ((WATER_SLOPE_CAPTURED_TILT2_GX - WATER_SLOPE_CAPTURED_TILT1_GX) * i) / WATER_SLOPE_CAPTURED_SWEEP_STEPS;
        const int gy =
            WATER_SLOPE_CAPTURED_TILT1_GY
            + ((WATER_SLOPE_CAPTURED_TILT2_GY - WATER_SLOPE_CAPTURED_TILT1_GY) * i) / WATER_SLOPE_CAPTURED_SWEEP_STEPS;
        water_slope_step_and_log(&real, gx, gy, "captured", i);
    }

    const long mass_after = water_slope_water_mass(&real);

    free(big);
    free(blocks);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE((int)mass_after, (int)mass_before,
                                             "the captured diagonal gravity change must never create water - "
                                             "soaking may only ever spend it");
}

/* --- the panel clock and heal against a real pour -----------------------
 *
 * Drives the real app_sand.c, touch pour and all, and times gfx_present()
 * three ways: 40 MHz, 80 MHz, and 80 MHz with sand's heal policy. The heal
 * is only worth having if the third stays clearly under the first. */

#include "app.h"

extern const app_t app_sand;
extern int sand_app_enter_running_for_test(void);
extern void sand_app_restore_colour_mode_for_test(int mode);
extern void sand_app_select_brush_for_test(int brush);

typedef enum {
    CLOCK_ROW_40,
    CLOCK_ROW_80,
    CLOCK_ROW_80_HEAL,
    CLOCK_ROW_COUNT,
} clock_row_t;

#define CLOCK_ROW_POUR_WARM_FRAMES 30
#define CLOCK_ROW_POUR_FRAMES      240
#define CLOCK_ROW_SETTLE_FRAMES    300
#define CLOCK_ROW_SETTLED_FRAMES   120
#define CLOCK_ROW_DT_MS            16

typedef struct {
    int64_t present_us;
    int64_t bytes;
    int64_t heal_bytes;
} clock_row_window_t;

static void
clock_row_frame(const input_t* in, int64_t* present_us) {
    if (gfx_full_redraw_pending()) {
        gfx_full_redraw_clear_pending();
        app_sand.invalidate();
    }
    app_sand.update(CLOCK_ROW_DT_MS, in);
    app_sand.frame(CLOCK_ROW_DT_MS, in);
    const int64_t t0 = esp_timer_get_time();
    gfx_present();
    *present_us += esp_timer_get_time() - t0;
}

static clock_row_window_t
clock_row_measure(int frames, bool pouring, int* frame_index) {
    gfx_reset_strip_send_counts();
    clock_row_window_t w = {0};
    for (int i = 0; i < frames; i++, (*frame_index)++) {
        input_t in = {0};
        in.down = pouring;
        in.pressed = pouring && *frame_index == 0;
        in.x = 150 + (*frame_index * 7) % 60;
        in.y = 120 + (*frame_index * 3) % 40;
        clock_row_frame(&in, &w.present_us);
    }
    w.present_us /= frames;
    w.bytes = gfx_get_bytes_sent() / frames;
    w.heal_bytes = gfx_get_heal_bytes_sent() / frames;
    return w;
}

static void
clock_row_run(int brush, clock_row_t row, clock_row_window_t* pour, clock_row_window_t* settled) {
    gfx_set_panel_clock_hz(row == CLOCK_ROW_40 ? GFX_PANEL_CLOCK_SLOW_HZ : GFX_PANEL_CLOCK_FAST_HZ);
    app_sand.enter();
    const int previous_mode = sand_app_enter_running_for_test();
    sand_app_select_brush_for_test(brush);
    if (row == CLOCK_ROW_80) {
        gfx_heal_set_budget(0);
    }

    int frame_index = 0;
    clock_row_measure(CLOCK_ROW_POUR_WARM_FRAMES, true, &frame_index);
    *pour = clock_row_measure(CLOCK_ROW_POUR_FRAMES, true, &frame_index);
    input_t lift = {0};
    lift.released = true;
    int64_t unused = 0;
    clock_row_frame(&lift, &unused);
    clock_row_measure(CLOCK_ROW_SETTLE_FRAMES, false, &frame_index);
    *settled = clock_row_measure(CLOCK_ROW_SETTLED_FRAMES, false, &frame_index);

    app_sand.exit();
    sand_app_restore_colour_mode_for_test(previous_mode);
    gfx_heal_restore_defaults();
}

static void
test_present_cost_at_40_mhz_80_mhz_and_80_mhz_with_heal_on_a_real_pour(void) {
    static const char* const brush_names[] = {"sand", "water"};
    static const char* const row_names[CLOCK_ROW_COUNT] = {"40", "80", "80+heal"};
    const two_core_scope_t core = two_core_scope_begin(true);
    const panel_clock_scope_t clock = panel_clock_scope_begin(GFX_PANEL_CLOCK_FAST_HZ);
    bool unhealed_row_sent_heal = false;

    for (int brush = 0; brush < 2; brush++) {
        for (int row = 0; row < CLOCK_ROW_COUNT; row++) {
            clock_row_window_t pour, settled;
            clock_row_run(brush, (clock_row_t)row, &pour, &settled);
            ESP_LOGI("device_tests",
                     "CLOCK_ROW %s pour %s MHz: present %lld us, %lld bytes (heal %lld) | settled: present %lld "
                     "us, %lld bytes (heal %lld)",
                     brush_names[brush], row_names[row], (long long)pour.present_us, (long long)pour.bytes,
                     (long long)pour.heal_bytes, (long long)settled.present_us, (long long)settled.bytes,
                     (long long)settled.heal_bytes);
            if (row != CLOCK_ROW_80_HEAL && pour.heal_bytes + settled.heal_bytes != 0) {
                unhealed_row_sent_heal = true;
            }
        }
    }
    panel_clock_scope_end(clock);
    two_core_scope_end(core);

    /* Asserted only after both scopes end: a failing assert longjmps out,
     * and a clock left at 40 MHz slows every later present in this boot. */
    TEST_ASSERT_FALSE_MESSAGE(unhealed_row_sent_heal, "only the heal row may send heal strips");
}

#endif /* DEVICE_BUILD */

/* --- suite -------------------------------------------------------------- */

#ifdef DEVICE_BUILD
/* Defined in app_sand.c, the hardware-facing entry point, which is not part
 * of the host build - hence DEVICE_BUILD around both this declaration and
 * the test. Declared here rather than in a header because one function does
 * not earn an app_sand.h and nothing else calls it. */
bool sand_app_alloc_selfcheck(size_t* out_largest_free, bool* out_impulses_ok);

/* Runs inside the suite on purpose, not before it. The question worth asking
 * is whether the app can be entered on a heap this suite has already worked
 * over, because that is the state someone actually finds the board in after
 * an autorun image finishes. */
static void
test_the_sand_app_can_still_allocate_everything_it_needs(void) {
    size_t largest_free = 0;
    bool impulses_ok = false;
    const bool ok = sand_app_alloc_selfcheck(&largest_free, &impulses_ok);

    ESP_LOGI("device_tests", "app alloc selfcheck: essential=%s impulses=%s largest_free=%u", ok ? "ok" : "FAILED",
             impulses_ok ? "ok" : "failed", (unsigned)largest_free);

    /* impulses are REPORTED, not asserted: the app treats a missing impulse
     * buffer as losing the blast mechanic rather than losing the app, so
     * failing the suite over it would overstate the damage. */
    TEST_ASSERT_TRUE_MESSAGE(ok, "the sand app could not allocate the grid and buffers a fresh entry "
                                 "needs - the simulation every frame-budget row in this file measures "
                                 "is one this device can no longer actually run. Read largest_free in "
                                 "the line above: the grid alone needs a contiguous 41,216 bytes, and "
                                 "this suite has just churned dozens of allocations that size");
}
#endif /* DEVICE_BUILD */

void
run_sand_perf_suite(void) {
    RUN_TEST(test_acid_bubbles_do_not_favour_one_wall);
    RUN_TEST(test_acid_bubbles_still_fire_once_the_block_is_asleep);
    RUN_TEST(test_the_soak_only_skip_dispatches_far_fewer_cells_than_a_full_walk);
    RUN_TEST(test_the_soak_only_skip_matches_the_full_walks_grid_exactly);
    RUN_TEST(test_the_soak_only_skip_hash_survives_ambient_two_core_state);

#ifdef DEVICE_BUILD
    /* Every budget test below pins its own mode now, so this is provenance,
     * not a dependency: whatever two_core_step_on was already at suite
     * entry - the ESP_PLATFORM boot default, or whatever the last suite
     * run in this boot left it at. */
    ESP_LOGI("device_tests", "run_sand_perf_suite: two_core_step_on=%d at entry", (int)sand_two_core_step_enabled());
    perf_unmet_targets = 0;
    RUN_TEST(test_the_sand_app_can_still_allocate_everything_it_needs);
    RUN_TEST(test_a_full_size_step_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_settled_sand_costs_almost_nothing);
    RUN_TEST(test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget);
    RUN_TEST(test_turning_a_settled_pool_to_landscape_fits_in_the_frame_budget);
    RUN_TEST(test_flipping_gravity_on_a_mixed_scene_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_water_fits_in_the_frame_budget);
    RUN_TEST(test_the_xtensa_counters_over_three_scenes);
    /* Ungated: the two gas movers compare through sand_set_gas_walk(), an
     * ordinary API, so this runs in every diagnostics build. */
    RUN_TEST(test_the_gas_random_walk_against_the_exhaustive_mover);
    RUN_TEST(test_two_core_step_against_the_serial_path_on_three_scenes);
    RUN_TEST(test_two_core_step_at_every_quality_grid_size);
    RUN_TEST(test_a_gravity_flip_on_every_material_at_once_stays_sane);
    RUN_TEST(test_the_gas_budget_rows_on_the_serial_path);
    /* test_fire_cascading_..._fits_in_the_frame_budget also runs, ambient,
     * from inside the row above (gas_ab_reporting suppresses its own
     * asserts there) - its budget only means something pinned here, at
     * its own standalone entry. */
    {
        const two_core_scope_t core = two_core_scope_begin(true);
        RUN_TEST(test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget);
        two_core_scope_end(core);
    }
    RUN_TEST(test_a_full_screen_of_fire_fits_in_the_frame_budget);
    RUN_TEST(test_four_liquids_reacting_at_once_fits_in_the_frame_budget);
    RUN_TEST(test_the_lava_stress_scene_fits_in_the_frame_budget);
    RUN_TEST(test_a_screen_of_smoke_and_steam_fits_in_the_frame_budget);
    RUN_TEST(test_pouring_water_onto_a_plant_bed_costs_more_than_steady_growth);
    RUN_TEST(test_a_growing_plant_bed_fits_in_the_frame_budget);
    RUN_TEST(test_the_wood_leaf_shading_on_a_grove);
    RUN_TEST(test_a_campfire_on_a_sand_bed_fits_in_the_frame_budget);
    /* Same dual use as the fire-cascade row above: also called ambient
     * from test_the_gas_budget_rows_on_the_serial_path, pinned here for
     * their own standalone budget. */
    {
        const two_core_scope_t core = two_core_scope_begin(true);
        RUN_TEST(test_turning_a_packed_screen_of_gas_fits_in_the_frame_budget);
        RUN_TEST(test_turning_a_half_screen_of_gas_fits_in_the_frame_budget);
        two_core_scope_end(core);
    }
    RUN_TEST(test_the_thermal_shock_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_boiler_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_wet_earth_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_water_over_lava_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_gas_ignition_vessel_logs_the_blast_stress);
    RUN_TEST(test_the_gunpowder_basin_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_plant_ruin_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_filling_basin_scene_fits_in_the_frame_budget);
    RUN_TEST(test_the_snowfall_scene_fits_in_the_frame_budget);
    RUN_TEST(test_pouring_the_plant_brush_fits_in_the_frame_budget);
    RUN_TEST(test_a_settled_plant_garden_fits_in_the_frame_budget);
    RUN_TEST(test_a_finished_tree_fits_in_the_frame_budget);
    RUN_TEST(test_pouring_water_into_a_landscape_sand_bed_fits_in_the_frame_budget);
    RUN_TEST(test_pouring_water_into_a_deep_landscape_bed_fits_in_the_frame_budget);
    RUN_TEST(test_pouring_sand_onto_a_landscape_sand_bed_fits_in_the_frame_budget);

    RUN_TEST(test_present_cost_against_a_falling_sand_scene);
    RUN_TEST(test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene);
    RUN_TEST(test_present_cost_against_the_lava_stress_scene);
    RUN_TEST(test_present_cost_against_the_thermal_shock_scene);
    RUN_TEST(test_present_cost_against_a_landscape_gas_over_sand_pile);
    RUN_TEST(test_present_cost_against_a_landscape_levelling_pool);
    RUN_TEST(test_present_cost_at_40_mhz_80_mhz_and_80_mhz_with_heal_on_a_real_pour);

    const bool two_core_before = sand_two_core_step_enabled();
    sand_set_two_core_step(true);
    ESP_LOGI("device_tests", "liquid pass tables: two-core chunks enabled");
    RUN_TEST(test_submerged_pile_settles_and_logs_the_pass_split);
    RUN_TEST(test_water_slope_pouring_water_logs_the_pass_split);
    RUN_TEST(test_water_slope_controls_log_the_pass_split);
    RUN_TEST(test_water_slope_gravity_flip_logs_a_per_step_table);
    RUN_TEST(test_water_slope_captured_scene_diagonal_flip_logs_a_per_step_table);
    sand_set_two_core_step(two_core_before);
    ESP_LOGI("device_tests", "PERF TARGET SUMMARY: %d unmet", perf_unmet_targets);
#endif
}

SUITE_REGISTER(run_sand_perf_suite);

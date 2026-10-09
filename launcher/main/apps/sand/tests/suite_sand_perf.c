/*
 * Portable suite: the falling-sand automaton - frame-budget performance
 * against the shared benchmark scenes, plus the app-level allocation
 * selfcheck and a couple of full-grid acid-bubble tests.
 *
 * DEVICE_BUILD-only, almost entirely: wall-clock frame-budget assertions
 * are meaningless on a host whose CPU speed bears no relation to the
 * device's, so nearly every test below only compiles into the on-device
 * selftest image, not the host runner, see each test's own #ifdef
 * DEVICE_BUILD guard and RUN_TEST line.
 *
 * Split out of suite_sand.c, which had grown
 * past 32,000 lines across 500+ tests. Shared fixtures and assertion helpers
 * live in suite_sand_common.{c,h}; the scene builders these frame-budget
 * tests measure live in suite_sand_scenes.{c,h}, see those headers.
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

#include "input/tilt.h" /* TILT_TAU_*_MS - the turn below follows the real
                       * filter shape rather than a straight line */
#include "apps/sand/material_palette.h"
#include "apps/sand/row_runs.h"
#include "apps/sand/sand.h"
#include "apps/sand/sand_priv.h"
#include "apps/sand/tests/suite_sand_common.h"
#include "apps/sand/tests/suite_sand_scenes.h"
#include "util/runtime/frame_watch.h"
#include "util/scalar/mathi.h"

/* Sand and dirt in equal amounts under water would soak; this instead pairs
 * sand against water across a settled stone-X divider that never lets the
 * two touch, so the reaction pass stays alive only on the wettable term
 * (MAT_SAND's own soaks!=0), the case sand_step_reactions()'s soak-only
 * skip exists for. Portable, not DEVICE_BUILD-only: the host regression
 * suite for that skip (suite_sand_dirt.c) reruns this same scene, and must
 * see exactly what the frame-budget test below measures. */
static void
build_mixed_gravity_flip_scene(sand_t* real, uint8_t* big, uint8_t* blocks) {
    sand_init(real, big, REAL_W, REAL_H, 17u);
    sand_enable_sleeping(real, blocks);

    const int sand_x1 = (REAL_W * 3) / 10;             /* ~30% from the left */
    const int water_x0 = REAL_W - ((REAL_W * 3) / 10); /* ~30% from the right */

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
    run_steps(real, 300, 0, 1000);
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
            if ((s->block_state[((size_t)by * (size_t)s->block_cols) + (size_t)bx] & BLOCK_LIQUID_NEAR) == 0) {
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
 * blocks instead, see its own soak_only comment.
 *
 * Runs the SAME scene and steps twice so "far fewer" reads against a
 * measured full-walk count, not a guess; the BLOCK_LIQUID_NEAR bound is likewise
 * summed fresh per step, since flipping gravity moves the marked blocks. */
static void
test_the_soak_only_skip_dispatches_far_fewer_cells_than_a_full_walk(void) {
    uint8_t* big;
    uint8_t* blocks;
    sand_test_grid_buffers_open(&big, &blocks, REAL_W, REAL_H);

    const int steps = 20;
    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);

    build_mixed_gravity_flip_scene(real, big, blocks);
    sand_reactions_force_full_walk(true);
    sand_reactions_cells_dispatched = 0;
    run_steps(real, steps, 0, -1000);
    const unsigned dispatched_full = sand_reactions_cells_dispatched;

    build_mixed_gravity_flip_scene(real, big, blocks);
    sand_reactions_force_full_walk(false);
    sand_reactions_cells_dispatched = 0;
    long near_bound = 0;
    for (int i = 0; i < steps; i++) {
        /* Sampled AFTER the step, not before: the liquid pass inside
         * sand_step() refreshes BLOCK_LIQUID_NEAR before reactions runs, so
         * the state reactions actually saw this step is the state left
         * behind at the end of it, not the one entering it. */
        sand_step(real, 0, -1000, 0);
        near_bound += liquid_near_cell_bound(real);
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
    free(real);
}

/* THE FINGERPRINT: this exact scene and step count is also
 * report_fingerprint.sh's device/host equivalence anchor, see its own
 * top comment for why 20 flip steps and this hash. Kept here beside the
 * scene it hashes rather than duplicated. */
#define MIXED_FLIP_20_STEP_HASH 0x6a6aa1cfu

/* Equivalence half of the regression above: the soak-only skip must be
 * byte-identical to the reference full walk. Pinned to serial like
 * report_fingerprint.sh's own capture (grid_fingerprint.c) - two-core
 * stepping is deliberately a different hash (Sand-Simulation.md). */
static void
test_the_soak_only_skip_matches_the_full_walks_grid_exactly(void) {
    uint8_t* big;
    uint8_t* blocks;
    sand_test_grid_buffers_open(&big, &blocks, REAL_W, REAL_H);

    const bool two_core_before = sand_two_core_step_enabled();
    sand_set_two_core_step(false);

    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);
    build_mixed_gravity_flip_scene(real, big, blocks);

    sand_reactions_force_full_walk(true);
    run_steps(real, 20, 0, -1000);
    const uint32_t full_hash = grid_hash(big, (size_t)REAL_W * (size_t)REAL_H);

    build_mixed_gravity_flip_scene(real, big, blocks);
    sand_reactions_force_full_walk(false);
    run_steps(real, 20, 0, -1000);
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
    free(real);
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

#ifdef DEVICE_BUILD
#include <stdlib.h>
#include "esp_cpu.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_debug.h"
#include "gfx/present/gfx_present.h"
#include "panel_clock_pin.h"
#include "util/runtime/frame_cost.h"
#include "util/runtime/timing.h"
#include "xtensa/xt_perf_consts.h"
#include "xtensa_perfmon_access.h"

/* Each pass's time for one step, summed over a window, and the split of the
 * window's dearest step. */
typedef struct {
    int64_t totals[6];
    int64_t peak[6];
    int64_t peak_total;
    int peak_impulses;
} pass_split_t;

static void log_pass_split(const char* name, int steps, int impulse_max, const pass_split_t* split, unsigned cap_hits);

/* Adds the step real just took to split. Returns whether it is the dearest
 * step so far. */
static bool
pass_split_add(pass_split_t* split, const sand_t* real) {
    const int64_t pass[6] = {real->pass_us.sweep_us, real->pass_us.liquid_us,    real->pass_us.float_us,
                             real->pass_us.gas_us,   real->pass_us.reactions_us, real->pass_us.impulses_us};
    int64_t total = 0;
    for (int j = 0; j < 6; j++) {
        split->totals[j] += pass[j];
        total += pass[j];
    }
    if (total <= split->peak_total) {
        return false;
    }
    memcpy(split->peak, pass, sizeof split->peak);
    split->peak_total = total;
    split->peak_impulses = real->impulse_count;
    return true;
}

static int64_t
time_steps(sand_t* real, int steps, int gx, int gy, int gz) {
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = timing_now_us();
    for (int i = 0; i < steps; i++) {
        sand_step(real, gx, gy, gz);
    }
    const int64_t per_step = (timing_now_us() - start) / steps;
    two_core_scope_end(core);
    return per_step;
}

static void
log_step_time(const char* scene, int64_t per_step) {
    ESP_LOGI("device_tests", "%s, %dx%d: %lld us per step", scene, REAL_W, REAL_H, (long long)per_step);
}

/* time_steps() under ordinary gravity that also sums each pass's own time
 * and keeps the dearest step's split, then logs both under `scene`. */
static int64_t
time_steps_split(sand_t* real, int steps, const char* scene) {
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = timing_now_us();
    pass_split_t split = {.peak_total = -1};
    for (int i = 0; i < steps; i++) {
        sand_step(real, 0, 1000, 0);
        (void)pass_split_add(&split, real);
    }
    const int64_t per_step = (timing_now_us() - start) / steps;
    two_core_scope_end(core);

    log_step_time(scene, per_step);
    log_pass_split(scene, steps, real->impulse_max, &split, real->impulse_cap_hits);
    return per_step;
}

/* What a timed row does to its scene before step i, if anything. */
typedef void (*step_feed_fn)(sand_t* real, int i);

/* Untimed: feeds real, then steps it, `steps` times under (gx, gy). */
static void
run_fed_steps(sand_t* real, int steps, int gx, int gy, step_feed_fn feed) {
    for (int i = 0; i < steps; i++) {
        feed(real, i);
        sand_step(real, gx, gy, 0);
    }
}

/* time_steps() with feed() run before each step - inside the mean, outside
 * the step's own timer - and the worst single step written to *worst_out. */
static int64_t
time_fed_steps(sand_t* real, int steps, int gx, int gy, step_feed_fn feed, int64_t* worst_out) {
    int64_t worst = 0;
    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = timing_now_us();
    for (int i = 0; i < steps; i++) {
        feed(real, i);
        const int64_t t0 = timing_now_us();
        sand_step(real, gx, gy, 0);
        const int64_t took = timing_now_us() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (timing_now_us() - start) / steps;
    two_core_scope_end(core);
    *worst_out = worst;
    return per_step;
}

static void
log_step_and_worst(const char* scene, int64_t per_step, int64_t worst) {
    ESP_LOGI("device_tests", "%s, %dx%d: %lld us per step, worst single step %lld us", scene, REAL_W, REAL_H,
             (long long)per_step, (long long)worst);
}

static void
feed_plant_ruin_acid(sand_t* real, int i) {
    if (i % PLANT_RUIN_ACID_EVERY == 0) {
        plant_ruin_acid_pour(real);
    }
}

static void
feed_filling_basin(sand_t* real, int i) {
    if (i % FILLING_BASIN_POUR_EVERY == 0) {
        filling_basin_pour(real);
    }
}

static void
feed_snowfall_drift(sand_t* real, int i) {
    if (i % SNOWFALL_DRIFT_EVERY == 0) {
        snowfall_drift(real);
    }
}

static int perf_unmet_targets;
static bool gas_ab_reporting;

/* The ceiling of a row is its goal x 1.15, rounded up to 10 us; the
 * present-cost ceilings follow the same rule with the panel clock pinned. */

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

/* The REAL_W x REAL_H board a timed row runs on, sleeping. real_board_close()
 * frees all of it but the sand_t, which the row frees itself. */
typedef struct {
    uint8_t* big;
    uint8_t* blocks;
    impulse_t* impulses;
} real_board_t;

/* Armed the way a shipped board is, see board_bookkeeping_open(). */
static sand_t*
real_board_open(real_board_t* b, uint32_t seed) {
    b->impulses = NULL;
    sand_t* const real = sand_test_grid_open(&b->big, &b->blocks, REAL_W, REAL_H, seed);
    board_bookkeeping_open(real);
    return real;
}

/* real_board_open() painted by a builder that initialises the grid itself. */
static sand_t*
real_board_open_built(real_board_t* b, void (*build)(sand_t*, uint8_t*, uint8_t*)) {
    b->impulses = NULL;
    sand_test_grid_buffers_open(&b->big, &b->blocks, REAL_W, REAL_H);
    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);
    build(real, b->big, b->blocks);
    board_bookkeeping_open(real);
    return real;
}

/* At the app's rates with a queue of impulse_max impulses, allocated ahead of
 * the sand_t. Unlike its siblings it does not arm the board: a row that wants
 * the bookkeeping opens it. */
static sand_t*
real_board_open_impulses(real_board_t* b, int impulse_max, uint32_t seed) {
    sand_test_grid_buffers_open(&b->big, &b->blocks, REAL_W, REAL_H);
    b->impulses = malloc((size_t)impulse_max * sizeof *b->impulses);
    TEST_ASSERT_NOT_NULL(b->impulses);

    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);
    sand_init(real, b->big, REAL_W, REAL_H, seed);
    sand_enable_sleeping(real, b->blocks);
    use_app_rates(real);
    sand_enable_impulses(real, b->impulses, impulse_max);
    return real;
}

static void
real_board_close(real_board_t* b) {
    board_bookkeeping_close();
    free(b->big);
    free(b->blocks);
    free(b->impulses);
}

/* The end of a plain row: times `steps` steps under (0, gy), logs them under
 * `scene` and frees the board. Returns the mean, which the row holds to its
 * budget itself - report_performance.py reads each row's ceiling from the
 * perf_target() call in its own body. */
static int64_t
finish_timed_row(sand_t* real, real_board_t* b, int steps, int gy, const char* scene) {
    const int64_t per_step = time_steps(real, steps, 0, gy, 0);
    log_step_time(scene, per_step);
    real_board_close(b);
    free(real);
    return per_step;
}

/* finish_timed_row() for a row fed before each step, under ordinary gravity,
 * that logs its worst step too. */
static int64_t
finish_fed_row(sand_t* real, real_board_t* b, int steps, step_feed_fn feed, const char* scene) {
    int64_t worst = 0;
    const int64_t per_step = time_fed_steps(real, steps, 0, 1000, feed, &worst);
    log_step_and_worst(scene, per_step, worst);
    real_board_close(b);
    free(real);
    return per_step;
}

/* A row that differs from others only by data: `build` painted on a fresh
 * board, settled settle_steps under ordinary gravity, then timed over `steps`
 * - fed by `feed` before each one when it is set. */
typedef struct {
    uint32_t seed;
    bool app_rates;
    bool soak;
    void (*build)(sand_t* s);
    int settle_steps;
    int steps;
    step_feed_fn feed;
    const char* scene;
} settled_row_t;

static int64_t
run_settled_row(const settled_row_t* row) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, row->seed);
    if (row->app_rates) {
        use_app_rates(real);
    }
    if (row->soak) {
        sand_set_soak(real, SAND_SOAK_PER_MATERIAL);
    }
    row->build(real);
    run_steps(real, row->settle_steps, 0, 1000);

    if (row->feed != NULL) {
        return finish_fed_row(real, &b, row->steps, row->feed, row->scene);
    }
    return finish_timed_row(real, &b, row->steps, 1000, row->scene);
}

/* The gas rows run on both paths and the app runs one core at lower
 * qualities, so each path has its own goal and ceiling. */
static void
perf_target_by_core(const char* two_core_name, const char* one_core_name, int64_t measured_us, int64_t two_core_goal_us,
                    int64_t two_core_ceiling_us, int64_t one_core_goal_us, int64_t one_core_ceiling_us) {
    if (sand_two_core_step_enabled()) {
        perf_target(two_core_name, measured_us, two_core_goal_us, two_core_ceiling_us);
    } else {
        perf_target(one_core_name, measured_us, one_core_goal_us, one_core_ceiling_us);
    }
}

/* The worst case: every cell on the screen moving at once. Unrelated code
 * shifting the flash layout can move this row between builds with no work
 * changed, so confirm a miss with a seeded perf_compare.sh run before reading
 * it as a regression. */
#define FULL_STEP_BUDGET_US 7290

/* Goal = worst of a 5-run board capture + 1% (layout drift between images
 * moves rows by tenths of a percent), rounded up to 10 us; a change must beat
 * it. The ceiling is a regression guard. */

static void
test_a_full_size_step_fits_in_the_frame_budget(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    TEST_ASSERT_NOT_NULL_MESSAGE(big, "the real grid must fit in what the framebuffer leaves behind");

    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);
    build_full_size_step_scene(real, big);
    board_bookkeeping_open(real);
    const int grains = sand_count(real);

    const int steps = 10;
    const int64_t per_step = time_steps(real, steps, 0, 1, 0);

    ESP_LOGI("device_tests", "sand_step on %dx%d with %d grains: %lld us", REAL_W, REAL_H, grains, (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(grains, sand_count(real), "the full-size grid must conserve grains too");

    board_bookkeeping_close();
    free(big);

    perf_target("full-size step", per_step, FULL_STEP_BUDGET_US, 8380);
    free(real);
}

/* A frame-budget fixture that asks for two cores and measures one reads as a
 * two-core number and is not one - which is what every fixture here did
 * before it armed a board the way a shipped one is armed. A busy full-size
 * step has to reach the split path, and a count of dispatches says so
 * without a clock. */
static void
test_a_frame_budget_board_really_reaches_the_split_path(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    TEST_ASSERT_NOT_NULL(big);

    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);
    build_full_size_step_scene(real, big);
    board_bookkeeping_open(real);

    const two_core_scope_t core = two_core_scope_begin(true);
    memset(sand_split_dispatches, 0, sizeof sand_split_dispatches);
    sand_step(real, 0, 1000, 0);
    const unsigned dispatched = sand_split_dispatches[SAND_SPLIT_SLOT_SWEEP];
    two_core_scope_end(core);

    board_bookkeeping_close();
    free(big);

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0u, dispatched,
                                          "a frame-budget fixture must arm a board its sweep can be split over");
    free(real);
}

/* Half a w x h screen of water, dropped in as an uneven slab so it is
 * genuinely flowing rather than already settled - the expensive case. */
static void
build_quality_water_pour_scene(sand_t* real, uint8_t* big, uint8_t* blocks, int w, int h) {
    sand_init(real, big, w, h, 11u);
    sand_enable_sleeping(real, blocks);
    sand_fill_box(real, w / 4, 0, (w * 3) / 4, h / 2, CELL_MAKE(MAT_WATER, MASS_MAX));
}

static void
build_water_scene(sand_t* real, uint8_t* big, uint8_t* blocks) {
    build_quality_water_pour_scene(real, big, blocks, REAL_W, REAL_H);
}

static int64_t
water_scene_us_per_step(void) {
    real_board_t b;
    sand_t* const real = real_board_open_built(&b, build_water_scene);

    const int steps = 20;
    const int64_t per_step = time_steps(real, steps, 0, 1000, 0);

    real_board_close(&b);
    free(real);
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
     * sustained, argue the budget down instead of up. */
    perf_target("screen-wide water collapse", per_step, 18480, 21250);
}

#ifdef DEVICE_BUILD
static void
build_fire_scene(sand_t* real, uint8_t* big, uint8_t* blocks) {
    sand_init(real, big, REAL_W, REAL_H, 19u);
    sand_enable_sleeping(real, blocks);
    sand_fill_box(real, 0, 0, REAL_W, REAL_H, FIRE);
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
            uint8_t* big;
            uint8_t* blocks;
            sand_test_grid_buffers_open(&big, &blocks, REAL_W, REAL_H);

            sand_t* const real = malloc(sizeof *real);
            TEST_ASSERT_NOT_NULL(real);
            build_fire_scene(real, big, blocks);
            run_steps(real, FIRE_WARMUP_STEPS, 0, 1000);

            sand_set_gas_walk(real, arm == 1);
            const int64_t start = timing_now_us();
            sand_step(real, 0, 1000, 0);
            const int64_t took = timing_now_us() - start;

            free(real);
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
    uint8_t* big;
    uint8_t* blocks;
    sand_test_grid_buffers_open(&big, &blocks, REAL_W, REAL_H);
    uint8_t* stamps = malloc(sand_step_stamp_bytes(REAL_W, REAL_H));
    TEST_ASSERT_NOT_NULL(stamps);

    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);
    build(real, big, blocks);
    /* Without these the split path is never ready and both arms time the
     * serial walk, so the comparison reads as no gain from a second core. */
    sand_enable_step_stamps(real, stamps);
    void* scratch = lane_scratch_open(real);

    const two_core_scope_t core = two_core_scope_begin(two_core);
    /* What this row prices is the split, so the split arm asks for it by
     * name: a scene the shipped decision declines would otherwise read
     * serial against serial. */
    const sand_chunk_share_t share =
        sand_chunk_share_for_test(two_core ? SAND_CHUNK_SHARE_ALWAYS : SAND_CHUNK_SHARE_AUTO);
    const int steps = 20;
    const int64_t start = timing_now_us();
    for (int i = 0; i < steps; i++) {
        sand_step(real, 0, gy, 0);
    }
    *out_per_step = (timing_now_us() - start) / steps;
    (void)sand_chunk_share_for_test(share);
    two_core_scope_end(core);
    collect_core1_lane();

    free(scratch);
    free(stamps);
    free(real);
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
    sand_fill_box(real, w / 4, 0, (w * 3) / 4, h / 2, SAND_FIRST_SHADE);
}

static void
build_quality_gas_scene(sand_t* real, uint8_t* big, uint8_t* blocks, int w, int h) {
    sand_init(real, big, w, h, 17u);
    sand_enable_sleeping(real, blocks);
    sand_fill_box(real, 0, h / 2, w, h, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
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
    sand_fill_box(real, w / 4, (h * 2) / 3, (w * 3) / 4, h, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
}

static quality_bench_t
time_two_core_quality_scene(const quality_grid_t* quality, quality_scene_fn build, bool two_core) {
    const int w = GFX_WIDTH / quality->cell;
    const int h = GFX_HEIGHT / quality->cell;
    uint8_t* big;
    uint8_t* blocks;
    sand_test_grid_buffers_open(&big, &blocks, w, h);

    uint8_t* stamps = malloc(sand_step_stamp_bytes(w, h));
    TEST_ASSERT_NOT_NULL(stamps);

    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);
    build(real, big, blocks, w, h);
    /* Same reason as time_two_core_arm() above: no stamps and no lane
     * scratch means no split path to time. */
    sand_enable_step_stamps(real, stamps);
    void* scratch = lane_scratch_open(real);

    quality_bench_t out = {0};
    const two_core_scope_t core = two_core_scope_begin(two_core);
    /* Same reason as time_two_core_arm(). */
    const sand_chunk_share_t share =
        sand_chunk_share_for_test(two_core ? SAND_CHUNK_SHARE_ALWAYS : SAND_CHUNK_SHARE_AUTO);
    const int steps = 20;
    const int64_t start = timing_now_us();
    for (int i = 0; i < steps; i++) {
        sand_step(real, 0, 1000, 0);
        out.parallel_us +=
            real->pass_us.sweep_us + real->pass_us.liquid_us + real->pass_us.float_us + real->pass_us.gas_us;
        out.total_us += real->pass_us.sweep_us + real->pass_us.liquid_us + real->pass_us.float_us + real->pass_us.gas_us
                        + real->pass_us.reactions_us + real->pass_us.impulses_us;
    }
    out.per_step_us = (timing_now_us() - start) / steps;
    (void)sand_chunk_share_for_test(share);
    two_core_scope_end(core);
    collect_core1_lane();

    out.chunks = sand_chunk_rows(real, SAND_CHUNK_TRAVEL_OTHER) * sand_chunk_cols(real, SAND_CHUNK_TRAVEL_OTHER);
    free(scratch);
    free(stamps);
    free(big);
    free(blocks);
    free(real);
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

#endif /* DEVICE_BUILD */

/*
 * the chunk layout sweep ---------------------------------------------- *
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
    int w, h;
    int sides[SWEEP_SIDES][2]; /* a {0, 0} entry ends the list */
} sweep_quality_t;

/* Both cuts a quality now ships, the square one they replaced, and the same
 * two widths at twice the chunk height - which is what asks whether the
 * 17-cell height every winner so far has is the floor doing the work or a
 * height that happens to win. A finer cut at 17 is not on the list because
 * SAND_CHUNKS_MAX leaves no room for one: ULTRA at 17 rows deep has 14 chunk
 * rows, so four chunk columns is already 56 of the 64. */
static const sweep_quality_t sweep_qualities[] = {
    {"ULTRA", 184, 224, {{92, 17}, {47, 17}, {45, 45}, {92, 34}, {47, 34}}},
    {"HIGH", 122, 149, {{61, 17}, {25, 17}, {30, 30}, {61, 34}, {25, 34}}},
    {"NORMAL", 92, 112, {{46, 17}, {23, 17}, {22, 22}, {46, 34}, {23, 34}}},
    /* Three apiece: at these grids every layout measured so far was slower
     * than one core, which is why they ship serial, and these exist only to
     * confirm that it stays so. */
    {"LOW", 61, 74, {{30, 17}, {17, 17}, {30, 34}}},
    {"VERY LOW", 46, 56, {{23, 17}, {17, 17}, {23, 28}}},
};

/* Portable, so an unplannable cut fails on a laptop: the board would fall
 * back to one lane and the line would read as a measurement of the side it
 * asked for. */
static void
test_every_swept_chunk_layout_is_one_the_planner_takes(void) {
    for (size_t qi = 0; qi < sizeof sweep_qualities / sizeof sweep_qualities[0]; qi++) {
        const sweep_quality_t* const q = &sweep_qualities[qi];

        for (int di = 0; di < SWEEP_SIDES && q->sides[di][0] != 0; di++) {
            sand_chunk_plan_t plan;
            char why[160];

            snprintf(why, sizeof why, "%s %dx%d: %dx%d is not a cut sand_chunk_plan() takes", q->name, q->w, q->h,
                     q->sides[di][0], q->sides[di][1]);
            TEST_ASSERT_TRUE_MESSAGE(q->sides[di][0] >= SAND_CHUNK_SIDE_MIN && q->sides[di][1] >= SAND_CHUNK_SIDE_MIN,
                                     why);
            TEST_ASSERT_TRUE_MESSAGE(sand_chunk_plan(&plan, q->w, q->h, q->sides[di][0], q->sides[di][1], 0, 0), why);
        }
    }
}

/* Every side each quality now ships, so a cut can never leave the sweep's own
 * list without the comparison it is ranked against going with it. */
static void
test_the_sweep_measures_both_cuts_every_quality_ships(void) {
    for (size_t qi = 0; qi < sizeof sweep_qualities / sizeof sweep_qualities[0]; qi++) {
        const sweep_quality_t* const q = &sweep_qualities[qi];

        for (int travel = 0; travel < SAND_CHUNK_TRAVEL_CLASSES; travel++) {
            int side_x, side_y, found = 0;
            char why[160];

            sand_chunk_table_sides(q->w, q->h, (sand_chunk_travel_t)travel, &side_x, &side_y);
            for (int di = 0; di < SWEEP_SIDES && q->sides[di][0] != 0; di++) {
                found += (q->sides[di][0] == side_x && q->sides[di][1] == side_y);
            }
            snprintf(why, sizeof why, "%s class %d: the shipped %dx%d is not on the sweep's own list", q->name, travel,
                     side_x, side_y);
            TEST_ASSERT_EQUAL_INT_MESSAGE(1, found, why);
        }
    }
}

static int
sweep_cells_of(const sand_t* s, material_id_t material) {
    int n = 0;

    for (int i = 0; i < s->w * s->h; i++) {
        const cell_t c = s->cells[i];
        n += (!CELL_IS_EMPTY(c) && CELL_MATERIAL(c) == material);
    }
    return n;
}

/* A benchmark has to be shown to run what it claims to measure. The open
 * block must still be climbing through the measured window, and the sealed
 * box must hold a saturated pocket through it - the two halves of a gas step,
 * a walk with somewhere to go and a spread with nothing but gaps to hunt. */
static void
test_the_sweeps_gas_scenes_put_work_in_both_gas_passes(void) {
    static const int gx[] = {0, 1000};
    static const int gy[] = {1000, 0};

    enum { GW = 92, GH = 112 };

    for (size_t g = 0; g < sizeof gx / sizeof gx[0]; g++) {
        uint8_t* column = malloc(GW * GH);
        uint8_t* box = malloc(GW * GH);
        uint8_t* before = malloc(GW * GH);
        TEST_ASSERT_NOT_NULL(column);
        TEST_ASSERT_NOT_NULL(box);
        TEST_ASSERT_NOT_NULL(before);

        sand_t cs, bs;
        sand_init(&cs, column, GW, GH, SWEEP_SEED);
        sand_init(&bs, box, GW, GH, SWEEP_SEED);
        const int cw = build_layout_gas_column_scene(&cs);
        const int bw = build_layout_gas_box_scene(&bs);
        const int cells_before = sweep_cells_of(&cs, MAT_GAS);
        const int packed_before = sweep_cells_of(&bs, MAT_GAS);

        run_steps(&cs, cw, gx[g], gy[g]);
        run_steps(&bs, bw, gx[g], gy[g]);
        memcpy(before, column, GW * GH);
        for (int i = 0; i < SWEEP_STEPS; i++) {
            sand_step(&cs, gx[g], gy[g], 0);
            sand_step(&bs, gx[g], gy[g], 0);
        }

        char why[160];
        snprintf(why, sizeof why, "gravity %d,%d", gx[g], gy[g]);
        TEST_ASSERT_TRUE_MESSAGE(memcmp(before, column, GW * GH) != 0, why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(cells_before, sweep_cells_of(&cs, MAT_GAS), why);
        TEST_ASSERT_EQUAL_INT_MESSAGE(packed_before, sweep_cells_of(&bs, MAT_GAS), why);
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(GW * GH / 2, packed_before, why);

        free(before);
        free(box);
        free(column);
    }
}

#ifdef DEVICE_BUILD

typedef struct {
    const char* name;
    int gx, gy;
} sweep_orient_t;

static const sweep_orient_t sweep_orients[] = {{"landscape", 1000, 0}, {"portrait", 0, 1000}};

typedef struct {
    const char* name;
    int (*build)(sand_t*);
} sweep_scene_t;

/* The last two put work in the gas walk and the gas spread, which round two
 * left unmeasured - its scenes had no gas in them, so the two gas passes were
 * ranked on nothing. */
static const sweep_scene_t sweep_scenes[] = {
    {"mixed-flip", build_layout_mixed_flip_scene},
    {"water", build_layout_water_scene},
    {"sand-only", build_layout_sand_only_scene},
    {"settling-pile", build_layout_settling_pile_scene},
    {"levelling-pool", build_layout_levelling_pool_scene},
    {"gas-column", build_layout_gas_column_scene},
    {"gas-box", build_layout_gas_box_scene},
};

typedef struct {
    sand_t s;
    uint8_t* cells;
    uint8_t* blocks;
    uint8_t* stamps;
    void* scratch;
} sweep_board_t;

static void
sweep_board_open(sweep_board_t* b, int w, int h) {
    const size_t blocks = sand_sleep_block_bytes(w, h);

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

/* The scene is painted with the split off, so every arm starts on the same
 * board, and the warm-up runs inside the arm: a board settled by one lane is
 * not the board two lanes settle. A split arm's instruction count sums both
 * cores, so only the solo arm prices the chunking itself. The hashed arm
 * takes the split's per-cell draws down the serial walk, so the hash is
 * priced apart from the chunking that needs it. */
typedef enum {
    SWEEP_ARM_SERIAL,
    SWEEP_ARM_SERIAL_HASHED,
    SWEEP_ARM_SOLO,
    SWEEP_ARM_SPLIT,
} sweep_arm_t;

static const char* const sweep_arm_names[] = {"serial", "serial-hashed", "solo", "split"};

typedef struct {
    int64_t sweep, liquid, gas, react;
} sweep_pass_us_t;

static void
sweep_pass_add(sweep_pass_us_t* into, const sand_t* s) {
    into->sweep += s->pass_us.sweep_us;
    into->liquid += s->pass_us.liquid_us + s->pass_us.float_us;
    into->gas += s->pass_us.gas_us;
    into->react += s->pass_us.reactions_us;
}

static void
sweep_cell(const sweep_quality_t* q, const sweep_scene_t* sc, const int* side, const sweep_orient_t* o,
           sweep_arm_t arm) {
    sweep_board_t b;

    sweep_board_open(&b, q->w, q->h);
    TEST_ASSERT_TRUE_MESSAGE(sand_chunk_side_for_test(side[0], side[1]), "every swept side must clear the floor");
    const int warm = sc->build(&b.s);
    const int chunks = ((q->w + side[0] - 1) / side[0]) * ((q->h + side[1] - 1) / side[1]);

    const two_core_scope_t core = two_core_scope_begin(arm >= SWEEP_ARM_SOLO);
    /* The cut is what this sweep ranks, so a chunk arm measures the chunks it
     * asked for: left to decide, a scene quiet enough or cut coarsely enough
     * to fill one lane takes the serial walk and the row reads as a
     * measurement of a layout nothing ran on. */
    const sand_chunk_share_t share = sand_chunk_share_for_test(SAND_CHUNK_SHARE_ALWAYS);
    sand_force_hashed_rng(arm == SWEEP_ARM_SERIAL_HASHED);
    sand_chunk_pass_set_driver_for_test(arm == SWEEP_ARM_SOLO ? SAND_CHUNK_PASS_SOLO : SAND_CHUNK_PASS_CORE1);
    run_steps(&b.s, warm, o->gx, o->gy);
    b.s.split_lane_aborts = 0;
    sweep_pass_us_t pass = {0};
    const int64_t start = timing_now_us();
    for (int i = 0; i < SWEEP_STEPS; i++) {
        sand_step(&b.s, o->gx, o->gy, 0);
        sweep_pass_add(&pass, &b.s);
    }
    const int64_t took = timing_now_us() - start;
    sand_chunk_pass_set_driver_for_test(SAND_CHUNK_PASS_CORE1);
    sand_force_hashed_rng(false);
    (void)sand_chunk_share_for_test(share);
    two_core_scope_end(core);
    collect_core1_lane();

    const int64_t per_step = took / SWEEP_STEPS;
    const int64_t timed = pass.sweep + pass.liquid + pass.gas + pass.react;
    const int64_t other = (took > timed) ? took - timed : 0;

    ESP_LOGI("device_tests",
             "CHUNK_SWEEP quality=%s grid=%dx%d side=%dx%d scene=%s orient=%s arm=%s us_per_step=%lld aborts=%u "
             "chunks=%d sweep_us=%lld liquid_us=%lld gas_us=%lld react_us=%lld other_us=%lld took_us=%lld steps=%d",
             q->name, q->w, q->h, side[0], side[1], sc->name, o->name, sweep_arm_names[arm], (long long)per_step,
             b.s.split_lane_aborts, chunks, (long long)(pass.sweep / SWEEP_STEPS),
             (long long)(pass.liquid / SWEEP_STEPS), (long long)(pass.gas / SWEEP_STEPS),
             (long long)(pass.react / SWEEP_STEPS), (long long)(other / SWEEP_STEPS), (long long)took, SWEEP_STEPS);

    (void)sand_chunk_side_for_test(0, 0);
    sweep_board_close(&b);
}

/* Settled is a grid that stopped changing, not a step count: a pile takes
 * many times longer to come to rest at the largest grid than the smallest,
 * and a count generous at one end reads the other still moving. Measured
 * under emulation, 240 steps left the middle grid at 70 times its own floor. */
#define SWEEP_FLOOR_QUIET 8
#define SWEEP_FLOOR_CAP   4000
#define SWEEP_FLOOR_STEPS 40

static int
sweep_floor_settle(sweep_board_t* b, size_t cells) {
    uint32_t last = 0;
    int quiet = 0;
    int steps = 0;

    while (steps < SWEEP_FLOOR_CAP && quiet < SWEEP_FLOOR_QUIET) {
        sand_step(&b->s, 0, 1000, 0);
        const uint32_t now = grid_hash(b->cells, cells);
        quiet = (now == last) ? quiet + 1 : 0;
        last = now;
        steps++;
    }
    return steps;
}

static int64_t
sweep_floor_arm(const sweep_quality_t* q, bool two_core, int* out_settle_steps) {
    sweep_board_t b;

    sweep_board_open(&b, q->w, q->h);
    (void)build_layout_settling_pile_scene(&b.s);

    /* The shipped cut, named rather than left to the table: this pulls
     * gravity, so the class is the one every quality's second column holds,
     * and a side asked for by name is also what carries the two smallest
     * grids past SAND_CHUNK_SPLIT_MIN_CELLS - without it the split arm here
     * would be a second serial arm. */
    int side_x, side_y;
    sand_chunk_table_sides(q->w, q->h, SAND_CHUNK_TRAVEL_OTHER, &side_x, &side_y);
    TEST_ASSERT_TRUE(sand_chunk_side_for_test(side_x, side_y));

    const two_core_scope_t core = two_core_scope_begin(two_core);
    const int settled_in = sweep_floor_settle(&b, (size_t)q->w * (size_t)q->h);
    const int64_t start = timing_now_us();
    for (int i = 0; i < SWEEP_FLOOR_STEPS; i++) {
        sand_step(&b.s, 0, 1000, 0);
    }
    const int64_t per_step = (timing_now_us() - start) / SWEEP_FLOOR_STEPS;
    two_core_scope_end(core);
    collect_core1_lane();
    (void)sand_chunk_side_for_test(0, 0);

    sweep_board_close(&b);
    if (settled_in > *out_settle_steps) {
        *out_settle_steps = settled_in;
    }
    return per_step;
}

/* On the shipped side, not a swept one: this is the price of involving the
 * second core at all - four passes of prepare, merge, dispatch and join, and
 * the stamp clear - against a board with nothing left to hand it.
 * settle_steps at the cap means the pile never came to rest, so the two
 * numbers beside it are a transient and not the floor. */
static void
sweep_floor(const sweep_quality_t* q) {
    int settle_steps = 0;

    const int64_t serial = sweep_floor_arm(q, false, &settle_steps);
    const int64_t split = sweep_floor_arm(q, true, &settle_steps);

    ESP_LOGI("device_tests", "CHUNK_SWEEP_FLOOR quality=%s serial_us=%lld split_us=%lld settle_steps=%d", q->name,
             (long long)serial, (long long)split, settle_steps);
}

static void
sweep_quality(const sweep_quality_t* q) {
    sweep_floor(q);
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

/* By name, so reordering the table cannot quietly swap what a suite sweeps. */
static void
sweep_quality_named(const char* name) {
    for (size_t i = 0; i < sizeof sweep_qualities / sizeof sweep_qualities[0]; i++) {
        if (strcmp(sweep_qualities[i].name, name) == 0) {
            sweep_quality(&sweep_qualities[i]);
            return;
        }
    }
    TEST_FAIL_MESSAGE("no swept quality by that name");
}

static void
test_chunk_sweep_ultra(void) {
    sweep_quality_named("ULTRA");
}

static void
test_chunk_sweep_high(void) {
    sweep_quality_named("HIGH");
}

static void
test_chunk_sweep_normal(void) {
    sweep_quality_named("NORMAL");
}

static void
test_chunk_sweep_low(void) {
    sweep_quality_named("LOW");
}

static void
test_chunk_sweep_very_low(void) {
    sweep_quality_named("VERY LOW");
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
    real_board_t b;
    sand_t* const real = real_board_open(&b, 5u);

    /* Every cell full, so nothing can move anywhere. */
    sand_fill_box(real, 0, 0, REAL_W, REAL_H, SAND_FIRST_SHADE);
    sand_step(real, 0, 1, 0); /* one step to notice it is settled */

    const int steps = 50;
    const int64_t per_step = time_steps(real, steps, 0, 1, 0);

    ESP_LOGI("device_tests", "settled %dx%d grid: %lld us per step", REAL_W, REAL_H, (long long)per_step);

    const int grains = sand_count(real);
    real_board_close(&b);

    TEST_ASSERT_EQUAL_INT_MESSAGE(REAL_W * REAL_H, grains, "and nothing may have moved");
    /* The residual after the settled skip is unattributed. */
    perf_target("settled sand", per_step, 140, 170);
    free(real);
}

static void
test_flipping_gravity_on_a_settled_pile_fits_in_the_frame_budget(void) {
    /* Worst case pouring: all blocks wake at once. */
    real_board_t b;
    sand_t* const real = real_board_open(&b, 13u);

    /* A big pour: the middle half of the screen's width, filled from the
     * floor up to half the screen's height - wide enough to span many
     * block-columns, deliberately not the whole grid. */
    sand_fill_box(real, REAL_W / 4, REAL_H / 2, (REAL_W * 3) / 4, REAL_H, SAND_FIRST_SHADE);
    const int grains = sand_count(real);

    /* Let it fully settle first - every block should go to sleep, the
     * same state a real pile reaches between pours. */
    run_steps(real, 300, 0, 1000);

    /* Flip - straight up instead of straight down. */
    const int steps = 20;
    const int64_t per_step = time_steps(real, steps, 0, -1000, 0);

    ESP_LOGI("device_tests",
             "gravity flip on a %d-grain pile, %dx%d: %lld us "
             "per step",
             grains, REAL_W, REAL_H, (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(grains, sand_count(real), "flipping gravity must conserve grains too");

    real_board_close(&b);

    perf_guard("settled-pile gravity flip", per_step, 13320);
    free(real);
}

/* A 90-degree turn is the expensive case a gravity reversal is not:
 * reversing drops the body in place, while turning sideways makes the pool
 * re-level across the full grid width, which is what the cross-flow search
 * costs the most for. Swept one step per degree because the expensive
 * frames are the mid-re-level ones. The worst step is logged alongside the
 * asserted mean, since a mean alone can hide a spike. */
static void
test_turning_a_settled_pool_to_landscape_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 17u);

    /* About 40% of the grid, full width, resting on the floor - the user's
     * own "fill the screen to about 40% with water in portrait". */
    sand_fill_box(real, 0, (REAL_H * 3) / 5, REAL_W, REAL_H, CELL_MAKE(MAT_WATER, MASS_MAX));
    const int mass = (int)mass_of(real, REAL_W, REAL_H, MAT_WATER);

    /* Settle until every block sleeps - "with the water settled" is half the
     * reported condition, and a pool that is still moving would time
     * something else entirely. */
    run_steps(real, 300, 0, 1000);

    /* THE TURN. */
    const two_core_scope_t core = two_core_scope_begin(true);
    const int steps = 90;
    int64_t worst = 0;
    const int64_t start = timing_now_us();
    for (int i = 1; i <= steps; i++) {
        const int gx = (1000 * i) / steps;
        const int gy = 1000 - gx;
        const int64_t t0 = timing_now_us();
        sand_step(real, gx, gy, 0);
        const int64_t took = timing_now_us() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (timing_now_us() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "portrait->landscape turn on a settled %d-mass "
             "pool, %dx%d: %lld us per step, worst single "
             "step %lld us",
             mass, REAL_W, REAL_H, (long long)per_step, (long long)worst);

    const int mass_after = (int)mass_of(real, REAL_W, REAL_H, MAT_WATER);

    real_board_close(&b);

    TEST_ASSERT_EQUAL_INT_MESSAGE(mass, mass_after,
                                  "turning the board must move water, not create or destroy it - the "
                                  "cell COUNT changes as the pool re-levels, the mass must not");

    /* Perf-scoped, with the block at 16x32. */

    /* WORTH KNOWING BEFORE OPTIMISING THIS ROW: the impulse flight pass
     * never runs here at all - s->impulse_count is 0 for all 390 steps - and
     * a host pass map puts ~48% of the cost in cross-flow, ~1% reactions,
     * ~1.5% gas. */
    perf_target("settled pool landscape turn", per_step, 7670, 8820);
    free(real);
}

static void
log_gas_quarter_turn(const char* fill, const sand_t* real, int64_t per_step, int64_t worst) {
    ESP_LOGI("device_tests",
             "quarter turn on a %s screen of gas, %dx%d, %s: "
             "%lld us per step, worst single step %lld us, last gas pass %lld us",
             fill, REAL_W, REAL_H, sand_two_core_step_enabled() ? "two-core" : "serial", (long long)per_step,
             (long long)worst, (long long)real->pass_us.gas_us);
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
    const int64_t start = timing_now_us();
    for (int i = 0; i < steps; i++) {
        gx_q8 += (int32_t)(((int64_t)((1000 * 256) - gx_q8) * dt_ms) / (tau_ms + dt_ms));
        gy_q8 += (int32_t)(((int64_t)(0 - gy_q8) * dt_ms) / (tau_ms + dt_ms));

        const int64_t t0 = timing_now_us();
        sand_step(real, gx_q8 / 256, gy_q8 / 256, 0);
        const int64_t took = timing_now_us() - t0;
        if (took > worst) {
            worst = took;
        }
    }
    const int64_t per_step = (timing_now_us() - start) / steps;
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

/* Times the per-cell scan and wave over the real grid. Framebuffer
 * writes and wake-driven row paints are outside this measurement. */
static void
test_the_wood_leaf_shading_on_a_grove(void) {
    uint8_t* big = malloc(REAL_W * REAL_H);
    TEST_ASSERT_NOT_NULL(big);

    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);
    sand_init(real, big, REAL_W, REAL_H, 3u);
    build_tree_grove_scene(real);

    int8_t top5[5][2];
    int down = 0;
    material_wood_leaf_top5(0, 1000, &down, top5);

    int wood = 0, leaf = 0, near_leaf = 0, rows_lit = 0;
    unsigned sink = 0;

    const int64_t start = timing_now_us();
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
    const int64_t per_pass = (timing_now_us() - start) / 20;

    const int64_t c0 = timing_now_us();
    for (int rep = 0; rep < 20; rep++) {
        for (int y = 0; y < REAL_H; y++) {
            const uint8_t* row = big + ((size_t)y * REAL_W);
            for (int x = 0; x < REAL_W; x++) {
                sink += material_grain_hash(x, y);
                sink += (row[x] == MATX(MATX_LEAF)) || (row[x] == CELL_MAKE(MAT_WOOD, 0));
            }
        }
    }
    const int64_t control_pass = (timing_now_us() - c0) / 20;

    ESP_LOGI("device_tests",
             "wood/leaf shading on a grove: %lld us per full-grid pass, "
             "control %lld us, so the shading is %lld us "
             "(wood %d, of which %d beside a leaf; leaf %d; %d of %d rows "
             "carry foliage and so wake every gust tick) [%u]",
             (long long)per_pass, (long long)control_pass, (long long)(per_pass - control_pass), wood, near_leaf, leaf,
             rows_lit, REAL_H, sink & 1u);

    free(big);
    free(real);
}

/* Reported from play: the frame drops when water is poured onto the bed;
 * pacing is stable once plants are merely drinking from wet dirt. Same bed
 * and settle, timed over the steps immediately AFTER a fresh pour against a
 * quiet window, so the pair brackets what a player sees - the DIFFERENCE
 * between the two rows is the point, a number from either alone describes
 * half the experience. */
static void
test_pouring_water_onto_a_plant_bed_costs_more_than_steady_growth(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 11u);
    sand_set_soak(real, SAND_SOAK_PER_MATERIAL);
    build_plant_bed_scene(real);
    plant_bed_settle(real);

    const int steps = 20;

    const two_core_scope_t core = two_core_scope_begin(true);

    /* Steady first, from the same board the pour will start from - measuring
     * the pour first would leave the steady rows a wetter bed than the one
     * the other row times. */
    int64_t start = timing_now_us();
    for (int i = 0; i < steps; i++) {
        sand_step(real, 0, 1000, 0);
    }
    const int64_t steady = (timing_now_us() - start) / steps;

    plant_bed_rain(real);
    start = timing_now_us();
    for (int i = 0; i < steps; i++) {
        sand_step(real, 0, 1000, 0);
    }
    const int64_t poured = (timing_now_us() - start) / steps;
    two_core_scope_end(core);

    ESP_LOGI("device_tests",
             "plant bed pour: steady %lld us, first %d steps after a pour "
             "%lld us (%lld us more, %lld%%)",
             (long long)steady, steps, (long long)poured, (long long)(poured - steady),
             steady > 0 ? (long long)(((poured - steady) * 100) / steady) : 0);

    real_board_close(&b);
    free(real);
}

static void
test_a_growing_plant_bed_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 11u);
    sand_set_soak(real, SAND_SOAK_PER_MATERIAL);
    build_plant_bed_scene(real);
    plant_bed_settle(real);

    const int64_t per_step = finish_timed_row(real, &b, 20, 1000, "growing plant bed");
    perf_target("growing plant bed", per_step, 37180, 52840);
}

static void
test_a_campfire_on_a_sand_bed_fits_in_the_frame_budget(void) {
    /* Settled so the sand lands and the fire catches, and the timed steps are
     * a burning campfire rather than a scene still falling into place.
     * Perf-scoped, with the block at 16x32. */
    const int64_t per_step = run_settled_row(&(const settled_row_t){.seed = 23u,
                                                                    .build = build_campfire_scene,
                                                                    .settle_steps = 30,
                                                                    .steps = 20,
                                                                    .scene = "campfire on a sand bed"});
    perf_target("campfire on sand", per_step, 23520, 30010);
}

/* A tilted board is a different path, not a rotation of the same one:
 * equalise_gas() takes its spread direction from ring_dir(stable_index + 2), and
 * gas_run_t's carry runs only on an axis-aligned ray. Packed bounds the worst case
 * and is the only shape that fires the row skip; the half-screen scene below
 * is the realistic counterpart, and the pair is the point. */
static void
test_turning_a_packed_screen_of_gas_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 31u);

    build_smoke_and_steam_scene(real);
    const int total = REAL_W * REAL_H;

    int64_t worst = 0;
    const int64_t per_step = time_a_quarter_turn(real, 24, &worst);
    log_gas_quarter_turn("PACKED", real, per_step, worst);

    /* Read before the frees, asserted after - Unity longjmps out of a failing
     * assert, so an assert ahead of free() would leak ~41 KB on this device's
     * no-PSRAM heap. */
    const int count = sand_count(real);

    real_board_close(&b);

    /* Same condensation caveat as the smoke-and-steam row above, and more
     * of it: a turning board keeps stirring steam into fresh 2x2 patches,
     * so the loss is larger here and varies run to run. */
    if (!gas_ab_reporting) {
        TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(total - (total / 8), count,
                                                 "turning the board must not empty it - steam condensing into water "
                                                 "loses three cells a patch, but a packed screen that has shed an "
                                                 "eighth of itself is not the scene this row means to time");
    }
    perf_target_by_core("packed gas turn", "packed gas turn, one core", per_step, 99280, 114950, 109090, 126990);
    free(real);
}

static void
test_turning_a_half_screen_of_gas_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 31u);

    /* 40% of the grid, full width, against the ceiling - where gas ends up. */
    sand_fill_box(real, 0, 0, REAL_W, (REAL_H * 2) / 5, CELL_MAKE(MAT_GAS, 0));

    /* Settle first: the turn should start from a body at rest, not from a
     * field still finding its own shape. */
    run_steps(real, 60, 0, 1000);
    const int before = sand_count(real);

    int64_t worst = 0;
    const int64_t per_step = time_a_quarter_turn(real, 24, &worst);
    log_gas_quarter_turn("HALF", real, per_step, worst);

    const int after = sand_count(real);

    real_board_close(&b);

    if (!gas_ab_reporting) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(before, after,
                                      "turning the board must move gas, not create or destroy it - decay is "
                                      "off by default, so the cell count is conserved across the turn");
    }
    perf_target_by_core("half-screen gas turn", "half-screen gas turn, one core", per_step, 34670, 39870, 34790, 40010);
    free(real);
}

static void
test_flipping_gravity_on_a_mixed_scene_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open_built(&b, build_mixed_gravity_flip_scene);

    /* Flip - straight up instead of straight down. No grain-conservation
     * check: water's model can spread mass across cells, so sand_count()
     * legitimately changes; test_a_screen_of_water_fits_in_the_frame_budget
     * skips this same check for the same reason. */
    const int64_t per_step = finish_timed_row(real, &b, 20, -1000, "gravity flip on a mixed sand/water/stone-X scene");
    perf_target("mixed-scene gravity flip", per_step, 13800, 16860);
}

/* Counter 0 is cycles, seeded the way xtensa_perfmon_exec() seeds it (select
 * 0, mask 0xffff). kernelcnt 0 / tracelevel -1 is its own encoding for
 * "no interrupt-level filter" (xtensa_perfmon_config_t: negative tracelevel
 * means the filter is ignored) - every level counts, none excluded, which is
 * what a whole-step instrument needs. */
static void
measure_xtperf_event(const char* scene, sand_t* real, int gx, int gy, int gz, int steps,
                     const frame_cost_event_t* event, uint32_t* out_cycles, uint32_t* out_value) {
    TEST_ASSERT_TRUE_MESSAGE(frame_cost_counters_idle(),
                             "the counters are armed through frame_cost; disarm them first");
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

    if (strcmp(event->name, FRAME_COST_DEFAULT_EVENT) == 0 && value != 0) {
        const uint32_t cpi_x100 = (uint32_t)(((uint64_t)cycles * 100) / value);
        ESP_LOGI("xtperf", "scene=%s cycles_per_retired_insn=%u.%02u", scene, (unsigned)(cpi_x100 / 100),
                 (unsigned)(cpi_x100 % 100));
    }

    *out_cycles = cycles;
    *out_value = value;
}

/* build_full_size_step_scene() with no block map, as the full-size step row
 * times it. */
static void
build_full_size_step_scene_awake(sand_t* real, uint8_t* big, uint8_t* blocks) {
    (void)blocks;
    build_full_size_step_scene(real, big);
}

typedef struct {
    const char* name;
    void (*build)(sand_t* real, uint8_t* big, uint8_t* blocks);
    bool sleeping;
    int gy;
    int steps;
} xtperf_scene_t;

static const xtperf_scene_t xtperf_scenes[] = {
    {"mixed_flip", build_mixed_gravity_flip_scene, true, -1000, 20},
    {"water", build_water_scene, true, 1000, 20},
    {"full_step", build_full_size_step_scene_awake, false, 1, 10},
};

static void
run_xtperf_over_scene(const xtperf_scene_t* scene, uint64_t* total_cycles, uint64_t* total_insn) {
    for (int e = 0; e < frame_cost_event_count(); e++) {
        uint8_t* big;
        uint8_t* blocks = NULL;
        if (scene->sleeping) {
            sand_test_grid_buffers_open(&big, &blocks, REAL_W, REAL_H);
        } else {
            big = malloc(REAL_W * REAL_H);
            TEST_ASSERT_NOT_NULL(big);
        }

        sand_t* const real = malloc(sizeof *real);
        TEST_ASSERT_NOT_NULL(real);
        scene->build(real, big, blocks);

        uint32_t cycles = 0, value = 0;
        measure_xtperf_event(scene->name, real, 0, scene->gy, 0, scene->steps, frame_cost_event_at(e), &cycles, &value);

        free(real);
        free(big);
        free(blocks);

        *total_cycles += cycles;
        if (strcmp(frame_cost_event_at(e)->name, FRAME_COST_DEFAULT_EVENT) == 0) {
            *total_insn += value;
        }
    }
}

static void
log_mixed_scene_hashes(void) {
    uint8_t* big;
    uint8_t* blocks;
    sand_test_grid_buffers_open(&big, &blocks, REAL_W, REAL_H);
    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);
    build_mixed_gravity_flip_scene(real, big, blocks);
    ESP_LOGI("xtperf", "scene=mixed_flip hash settled=%08" PRIx32, grid_hash(big, REAL_W * REAL_H));
    for (int i = 0; i < 20; i++) {
        sand_step(real, 0, -1000, 0);
        if (i == 0 || i == 9 || i == 19) {
            ESP_LOGI("xtperf", "scene=mixed_flip hash flip%d=%08" PRIx32, i + 1, grid_hash(big, REAL_W * REAL_H));
        }
    }
    free(real);
    free(big);
    free(blocks);
}

/* An instrument, not a gate: asserts only that the counters moved at all -
 * the logged ratios are the point. Rebuilds each scene per event so every
 * window starts from the same deterministic state. Counters are per-CPU, so
 * the core is checked rather than assumed. */
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
    for (size_t i = 0; i < sizeof xtperf_scenes / sizeof xtperf_scenes[0]; i++) {
        run_xtperf_over_scene(&xtperf_scenes[i], &total_cycles, &total_insn);
    }
    two_core_scope_end(core);

    TEST_ASSERT_TRUE_MESSAGE(total_cycles > 0, "the cycle counter never moved across any scene or event");
    TEST_ASSERT_TRUE_MESSAGE(total_insn > 0, "the retired-instruction counter never moved across any scene");
}

/* Board banded with every material, reactive pairs touch, gravity inverted.
 * Catches combination costs. THE ASSERTION BELOW IS NOT A BUDGET. Replace
 * with a real figure from a device selftest run. */
static void
test_a_gravity_flip_on_every_material_at_once_stays_sane(void) {
    /* Without impulses, sand_explode() has nowhere to write and the
     * gunpowder patches below can never detonate - see
     * ALL_PAIRS_IMPULSE_MAX. */
    real_board_t b;
    sand_t* const real = real_board_open_impulses(&b, ALL_PAIRS_IMPULSE_MAX, 23u);
    board_bookkeeping_open(real);

    /* build_all_pairs_scene() (suite_sand_scenes.c) also plants the
     * deliberate gunpowder patches - the tiling alone scatters gunpowder
     * as one cell in nineteen, never enough to form the fuse's 2x2. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(1, ALL_PAIRS_SPAWN_COUNT,
                                         "the pattern below needs at least two materials to interleave");

    build_all_pairs_scene(real);

    /* Let it get going - long enough for the reactions to be under way and
     * the liquids to have found their levels, so the flip lands on a live
     * scene rather than a freshly painted one. */
    run_steps(real, 120, 0, 1000);

    const int64_t per_step = finish_timed_row(real, &b, 20, -1000, "gravity flip with every material at once");
    perf_target("all-material gravity flip", per_step, 80780, 94790);
}

static void
test_fire_cascading_through_a_full_screen_of_gas_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 17u);

    sand_fill_box(real, 0, 0, REAL_W, REAL_H, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
    sand_set(real, 0, 0, FIRE);
    const int total = REAL_W * REAL_H;

    const int64_t start = timing_now_us();
    sand_step(real, 0, 1000, 0);
    const int64_t elapsed = timing_now_us() - start;

    ESP_LOGI("device_tests",
             "fire cascading through a full %dx%d screen of "
             "gas, %s: %lld us for the one step, gas pass %lld us",
             REAL_W, REAL_H, sand_two_core_step_enabled() ? "two-core" : "serial", (long long)elapsed,
             (long long)real->pass_us.gas_us);

    if (!gas_ab_reporting) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(total, sand_count(real),
                                      "setup: cells must only ever convert material, never appear or "
                                      "vanish, across gas igniting into fire");
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(real, REAL_W - 1, REAL_H - 1)),
                                      "setup: the cascade must have reached the far corner - the whole "
                                      "grid must have ignited in this one step, or this is not "
                                      "actually measuring the worst case it claims to");
    }

    real_board_close(&b);

    /* A deliberately synthetic worst case, not comparable to the
     * plain-material rows. */
    perf_target_by_core("full-screen gas cascade", "full-screen gas cascade, one core", elapsed, 176900, 203430, 177680,
                        205160);
    free(real);
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
    real_board_t b;
    sand_t* const real = real_board_open(&b, 19u);

    sand_fill_box(real, 0, 0, REAL_W, REAL_H, FIRE);
    const int total = REAL_W * REAL_H;

    const int steps = 10;
    const int64_t per_step = time_steps(real, steps, 0, 1000, 0);

    ESP_LOGI("device_tests",
             "full %dx%d screen already fire, steady "
             "state: %lld us per step",
             REAL_W, REAL_H, (long long)per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, sand_count(real),
                                  "setup: a fully packed screen of same-density fire cannot "
                                  "displace, ignite, or smother anything - the count must not "
                                  "drift");

    real_board_close(&b);

    perf_target("full-screen fire", per_step, 67320, 77420);
    free(real);
}

static void
test_a_packed_landscape_screen_of_gas_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 31U);

    sand_fill_box(real, 0, 0, REAL_W, REAL_H, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = timing_now_us();
    sand_step(real, LANDSCAPE_GX, 0, 0);
    const int64_t elapsed = timing_now_us() - start;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "packed landscape gas, %dx%d: %lld us for one step, gas pass %lld us", REAL_W, REAL_H,
             (long long)elapsed, (long long)real->pass_us.gas_us);

    real_board_close(&b);

    perf_target("landscape packed gas", elapsed, 37870, 43560);
    free(real);
}

static void
test_a_full_landscape_screen_of_fire_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 19U);

    sand_fill_box(real, 0, 0, REAL_W, REAL_H, FIRE);
    const int total = REAL_W * REAL_H;

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = timing_now_us();
    const int steps = 10;
    for (int i = 0; i < steps; i++) {
        sand_step(real, LANDSCAPE_GX, 0, 0);
    }
    const int64_t per_step = (timing_now_us() - start) / steps;
    two_core_scope_end(core);

    log_step_time("full landscape fire", per_step);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, sand_count(real),
                                  "a fully packed screen of same-density fire cannot displace, ignite, or smother "
                                  "anything - the count must not drift");

    real_board_close(&b);

    perf_target("landscape full fire", per_step, 66970, 77040);
    free(real);
}

static void
test_fire_cascading_through_a_full_landscape_screen_of_gas_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 17U);

    sand_fill_box(real, 0, 0, REAL_W, REAL_H, CELL_MAKE(MAT_GAS, MATERIAL_VARIANTS - 1));
    sand_set(real, 0, 0, FIRE);
    const int total = REAL_W * REAL_H;

    const two_core_scope_t core = two_core_scope_begin(true);
    const int64_t start = timing_now_us();
    sand_step(real, LANDSCAPE_GX, 0, 0);
    const int64_t elapsed = timing_now_us() - start;
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "landscape fire cascade through %dx%d gas: %lld us, gas pass %lld us", REAL_W, REAL_H,
             (long long)elapsed, (long long)real->pass_us.gas_us);

    TEST_ASSERT_EQUAL_INT_MESSAGE(total, sand_count(real),
                                  "cells must only convert material while gas ignites into fire");
    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_FIRE, CELL_MATERIAL(sand_at(real, REAL_W - 1, REAL_H - 1)),
                                  "the cascade must reach the far corner in the measured step");

    real_board_close(&b);

    perf_target("landscape gas cascade", elapsed, 174170, 200300);
    free(real);
}

/* Four liquids of different density painted upside down
 * (build_four_liquid_scene(), shared with test_the_four_liquid_scene_
 * keeps_reacting_after_settling) so lava, acid, water and oil migrate past
 * each other the whole window instead of settling into inert bands. Also
 * the only benchmark here, besides the gravity-flip test above, running at
 * sand_set_mobility(SAND_MOBILITY_PER_MATERIAL), the setting app_sand.c
 * itself uses - so this holds the app's own liquid path to any real
 * ceiling. */
static void
test_four_liquids_reacting_at_once_fits_in_the_frame_budget(void) {
    /* Settled first - the same "let it get going" step as the every-material
     * flip test above, so the measured window lands on a live scene. */
    const int64_t per_step = run_settled_row(&(const settled_row_t){.seed = 29u,
                                                                    .app_rates = true,
                                                                    .build = build_four_liquid_scene,
                                                                    .settle_steps = 10,
                                                                    .steps = 20,
                                                                    .scene = "four liquids reacting at once"});
    perf_target("four reacting liquids", per_step, 64630, 75190);
}

static void
test_the_lava_stress_scene_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 37u);
    use_app_rates(real);

    build_lava_stress_scene(real);

    run_steps(real, 30, 0, 1000);

    const int64_t per_step = time_steps_split(real, 20, "lava stress scene");

    real_board_close(&b);

    perf_target("lava stress", per_step, 98290, 114740);
    free(real);
}

static void
test_a_screen_of_smoke_and_steam_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 31u);

    build_smoke_and_steam_scene(real);
    const int total = REAL_W * REAL_H;

    const int steps = 10;
    const int64_t per_step = time_steps(real, steps, 0, 1000, 0);

    log_step_time("screen of smoke and steam", per_step);

    /* Read before the frees below, asserted after: Unity longjmps out of a
     * failing assert, so an assert ahead of free() would skip it and leak
     * ~41 KB on this device's no-PSRAM heap. */
    const int count = sand_count(real);

    real_board_close(&b);

    /* host twin forces off with sand_set_condenses() due to budget pegged
     * with condensation running. Screen did not quietly empty into unmeasured
     * state. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(total - (total / 16), count,
                                             "setup: a screen of smoke and steam must still be essentially full "
                                             "at the end of the window - steam condensing into water loses three "
                                             "cells a patch, but losing an appreciable fraction of the board "
                                             "means it decayed into something else");
    perf_target("smoke and steam", per_step, 78970, 91710);
    free(real);
}

/* 480 glass compartments (build_thermal_shock_scene(), shared with
 * test_the_thermal_shock_scene_shatters_in_both_directions). No settling
 * steps: every ring starts strictly between the two shock thresholds and
 * touching from step 1, so the lattice is already at its most active the
 * moment it's painted. */
static void
test_the_thermal_shock_scene_fits_in_the_frame_budget(void) {
    /* Step count is fixed at 10 by the host guard beside this test (its own
     * comment covers the cullet timeline); the ceiling is chosen against
     * the device's 5-second task watchdog at that fixed count - raising the
     * count without minding the ceiling needs re-doing the bet. */
    const int64_t per_step = run_settled_row(&(const settled_row_t){.seed = 41u,
                                                                    .app_rates = true,
                                                                    .build = build_thermal_shock_scene,
                                                                    .steps = 10,
                                                                    .scene = "thermal shock lattice"});
    perf_target("thermal shock", per_step, 83240, 97690);
}

static void
test_the_boiler_scene_fits_in_the_frame_budget(void) {
    const int64_t per_step = run_settled_row(&(const settled_row_t){.seed = 43u,
                                                                    .app_rates = true,
                                                                    .build = build_boiler_scene,
                                                                    .settle_steps = 20,
                                                                    .steps = 30,
                                                                    .scene = "boiler scene"});
    perf_target("boiler", per_step, 19890, 26380);
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
    /* Perf-scoped, with the block at 16x32. */
    const int64_t per_step = run_settled_row(&(const settled_row_t){.seed = 53u,
                                                                    .app_rates = true,
                                                                    .soak = true,
                                                                    .build = build_wet_earth_scene,
                                                                    .settle_steps = 35,
                                                                    .steps = 30,
                                                                    .scene = "wet earth scene"});
    perf_target("wet earth", per_step, 29860, 37620);
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
    real_board_t b;
    sand_t* const real = real_board_open_impulses(&b, WATER_LAVA_IMPULSE_MAX, 59u);
    board_bookkeeping_open(real);

    build_water_over_lava_scene(real);

    const int64_t per_step = finish_timed_row(real, &b, 20, 1000, "water over lava scene");
    perf_target("water over lava", per_step, 133350, 154000);
}

static void
test_the_gas_ignition_vessel_logs_the_blast_stress(void) {
    real_board_t b;
    sand_t* const real = real_board_open_impulses(&b, GAS_IGNITION_VESSEL_IMPULSE_MAX, 71u);
    build_gas_ignition_vessel_scene(real);

    pass_split_t split = {.peak_total = -1};
    unsigned peak_blasts = 0;
    unsigned blasts_after_50 = 0;
    unsigned cap_hits_after_50 = 0;
    const two_core_scope_t core = two_core_scope_begin(true);
    for (int step = 1; step <= GAS_IGNITION_VESSEL_MEASURED_STEPS; step++) {
        const unsigned cap_before = real->impulse_cap_hits;
        sand_step(real, 0, 1000, 0);
        if (pass_split_add(&split, real)) {
            peak_blasts = real->explosions_this_step;
        }
        if (step <= 50) {
            ESP_LOGI("device_tests",
                     "gas ignition vessel step=%d blasts=%u live=%d dropped=%u sweep=%lld liq=%lld "
                     "flt=%lld gas=%lld react=%lld imp=%lld us",
                     step, real->explosions_this_step, real->impulse_count, real->impulse_cap_hits - cap_before,
                     (long long)real->pass_us.sweep_us, (long long)real->pass_us.liquid_us,
                     (long long)real->pass_us.float_us, (long long)real->pass_us.gas_us,
                     (long long)real->pass_us.reactions_us, (long long)real->pass_us.impulses_us);
        } else {
            blasts_after_50 += real->explosions_this_step;
            cap_hits_after_50 += real->impulse_cap_hits - cap_before;
        }
    }
    two_core_scope_end(core);

    ESP_LOGI("device_tests", "gas ignition vessel scene, %dx%d: steps=%d, post50 mean blasts=%u live=%d dropped=%u",
             REAL_W, REAL_H, GAS_IGNITION_VESSEL_MEASURED_STEPS,
             blasts_after_50 / (GAS_IGNITION_VESSEL_MEASURED_STEPS - 50), real->impulse_count, cap_hits_after_50);
    log_pass_split("gas ignition vessel scene", GAS_IGNITION_VESSEL_MEASURED_STEPS, real->impulse_max, &split,
                   real->impulse_cap_hits);
    ESP_LOGI("device_tests", "gas ignition vessel scene: peak blasts=%u", peak_blasts);

    real_board_close(&b);
    free(real);
}

/* The gunpowder basin scene (build_gunpowder_basin_scene(),
 * suite_sand_scenes.c), shared with the coverage test that proves the
 * chain-detonation really spans several bursts and reaches fuel
 * outside the vessel. */

/* NINETY STEPS, NO SETTLING - matching the coverage test exactly, so
 * this times the same run already proved to reach every path it
 * claims to. See GUNPOWDER_BASIN_MEASURED_STEPS's own comment
 * (suite_sand_scenes.c) for the timeline that window came from. */

static void
test_the_gunpowder_basin_scene_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open_impulses(&b, GUNPOWDER_BASIN_IMPULSE_MAX, 61u);
    board_bookkeeping_open(real);

    build_gunpowder_basin_scene(real);

    const int64_t per_step = time_steps_split(real, GUNPOWDER_BASIN_MEASURED_STEPS, "gunpowder basin scene");

    real_board_close(&b);

    perf_target("gunpowder basin", per_step, 26340, 34050);
    free(real);
}

/*
 * the interaction round's three scenes
 *
 * Picked from a 380-pairing arena rather than from the shape of the board.
 * Each builder (suite_sand_scenes.c) carries the measurement that earned it
 * a row, and the coverage test beside it proves the scene does that inside
 * the window timed here.
 */

/* Perf-scoped goals for the three plant-scene rows. */
#define PLANT_RUIN_BUDGET_US    55780
#define FILLING_BASIN_BUDGET_US 15560
#define SNOWFALL_BUDGET_US      32830

/* Perf-scoped; among the dearest scenes in the suite. */
#define PLANT_POUR_BUDGET_US    50820

/* What is left after a landed plant stopped arming the reaction pass (see
 * may_have_faller/faller_may_move in sand.h) is the sweep's own block scan. */
#define PLANT_IDLE_BUDGET_US    140

#define MATURE_TREE_BUDGET_US   18580

/* A grown plant bed with acid eating down to its roots on one side of a wall
 * and lava burning its canopy on the other (build_plant_ruin_scene(), shared
 * with test_the_plant_ruin_scene_eats_roots_and_burns_a_canopy). The acid
 * leads the lava by PLANT_RUIN_ACID_LEAD_STEPS because the two do not peak
 * together - see that constant. */
static void
test_the_plant_ruin_scene_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 11u);
    use_app_rates(real);
    sand_set_soak(real, SAND_SOAK_PER_MATERIAL);

    build_plant_ruin_scene(real);
    plant_bed_settle(real);
    run_fed_steps(real, PLANT_RUIN_ACID_LEAD_STEPS, 0, 1000, feed_plant_ruin_acid);
    plant_ruin_lava_pour(real);

    /* THE INTERACTION IS THE FINDING: the same bed, grown the same way, is
     * 68,076 us a step while it is merely drinking rain and 83,173 once acid
     * and lava arrive - 22% for the pours alone. */
    const int64_t per_step =
        finish_fed_row(real, &b, PLANT_RUIN_MEASURED_STEPS, feed_plant_ruin_acid, "plant ruin scene");
    perf_target("plant ruin", per_step, PLANT_RUIN_BUDGET_US, 71850);
}

/* Water running down a ramp into a pool (build_filling_basin_scene(), shared
 * with test_the_filling_basin_scene_runs_from_the_lip_to_the_pool) - the
 * companion to the free-falling slab above, on the same board and with a
 * comparable body of water, but settling rather than dropping into vacuum.
 * The slab row stays as it is so its figures remain comparable. */
static void
test_the_filling_basin_scene_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 17u);
    use_app_rates(real);

    build_filling_basin_scene(real);
    run_fed_steps(real, FILLING_BASIN_SETTLE_STEPS, 0, 1000, feed_filling_basin);

    /* WHAT THE PAIR SAYS, and it is the reason this row exists: the slab row
     * above measured 12,060 us a step in the same capture, this one 16,077.
     * A third more for the same board of water, purely for settling rather
     * than dropping into vacuum - so the row the water work is tuned on is
     * the cheaper of the two cases by 33%. */
    const int64_t per_step =
        finish_fed_row(real, &b, FILLING_BASIN_MEASURED_STEPS, feed_filling_basin, "filling basin scene");
    perf_target("filling basin", per_step, FILLING_BASIN_BUDGET_US, 18100);
}

/* Snow falling onto a bank that has already crusted, over sand and dirt
 * (build_snowfall_scene(), shared with test_the_snowfall_scene_holds_a_
 * crusting_bank_and_a_live_fall). Forced crust, see the builder's own
 * declaration for why a scene left at the shipped rate holds no ice at all
 * inside any window this file times. */
static void
test_the_snowfall_scene_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 23u);
    use_app_rates(real);
    sand_set_crust(real, CRUST_ROLL_MAX);

    build_snowfall_scene(real);
    run_steps(real, SNOWFALL_SETTLE_STEPS, 0, 1000);

    /* 63,371 us a step from a material that had no scene at all: about what
     * a growing plant bed costs, and dearer than a campfire. */
    const int64_t per_step = finish_fed_row(real, &b, SNOWFALL_MEASURED_STEPS, feed_snowfall_drift, "snowfall scene");
    perf_target("snowfall", per_step, SNOWFALL_BUDGET_US, 40530);
}

/* The plant brush poured onto damp earth (build_plant_pour_scene()), which no
 * other row reaches: every plant scene here grows a garden, and a grown tree
 * is anchored, so its support walk returns on the first neighbour.
 *
 * Timed from the first stamp rather than after a settle - a settled heap is
 * the plant bed row over again. */
static void
test_pouring_the_plant_brush_fits_in_the_frame_budget(void) {
    const int64_t per_step = run_settled_row(&(const settled_row_t){.seed = 11u,
                                                                    .soak = true,
                                                                    .build = build_plant_pour_scene,
                                                                    .settle_steps = PLANT_POUR_SETTLE_STEPS,
                                                                    .steps = PLANT_POUR_MEASURED_STEPS,
                                                                    .feed = plant_pour_stamp,
                                                                    .scene = "plant pour"});
    perf_target("plant pour", per_step, PLANT_POUR_BUDGET_US, 78730);
}

/* The same heap once it has stopped: the state a poured garden spends almost
 * all of its life in, and the one no other row measures. Every plant here is
 * landed or anchored, so the reaction pass has nothing it can do and the
 * number is whatever it costs to find that out. */
static void
test_a_settled_plant_garden_fits_in_the_frame_budget(void) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 11u);
    sand_set_soak(real, SAND_SOAK_PER_MATERIAL);
    build_dry_plant_heap_scene(real);

    run_fed_steps(real, PLANT_POUR_MEASURED_STEPS, 0, 1000, plant_pour_stamp);
    run_steps(real, PLANT_IDLE_SETTLE_STEPS, 0, 1000);

    const int64_t per_step = finish_timed_row(real, &b, 200, 1000, "settled plant garden");
    perf_target("settled plant garden", per_step, PLANT_IDLE_BUDGET_US, 170);
}

/* The maintainer's own case: a tree grown from seed on damp earth, with wood,
 * leaves and a root system, left until it has both stopped growing and drunk
 * the ground dry. Every other plant row here is chosen for something still
 * happening in it; this one is chosen for nothing happening, because that is
 * what a garden does for all but the first few hundred steps of its life. */
static void
test_a_finished_tree_fits_in_the_frame_budget(void) {
    const int64_t per_step = run_settled_row(&(const settled_row_t){.seed = 11u,
                                                                    .soak = true,
                                                                    .build = build_plant_bed_scene,
                                                                    .settle_steps = MATURE_TREE_SETTLE_STEPS,
                                                                    .steps = 200,
                                                                    .scene = "finished tree"});
    perf_target("finished tree", per_step, MATURE_TREE_BUDGET_US, 25040);
}

/* Every row above holds the board portrait, and the block shape behind the
 * settled-block skip was swept against exactly those rows. The board is
 * played LANDSCAPE, down grid +X - geometry in
 * suite_sand_scenes.h. Perf-scoped at block 16x32. */
#define LANDSCAPE_WATER_BUDGET_US      22380
#define LANDSCAPE_DEEP_WATER_BUDGET_US 24250
#define LANDSCAPE_SAND_BUDGET_US       6920

/* The measured steps carry on the pour the prime started. */
static void
feed_landscape_water(sand_t* real, int i) {
    landscape_water_pour(real, LANDSCAPE_PRIME_STEPS + i);
}

static void
feed_landscape_sand(sand_t* real, int i) {
    landscape_sand_pour(real, LANDSCAPE_PRIME_STEPS + i);
}

static int64_t
landscape_scene_us_per_step(sand_t* real, bool water, int64_t* worst_out) {
    const two_core_scope_t core = two_core_scope_begin(true);
    run_fed_steps(real, LANDSCAPE_PRIME_STEPS, LANDSCAPE_GX, 0, water ? landscape_water_pour : landscape_sand_pour);
    const int64_t per_step = time_fed_steps(real, LANDSCAPE_MEASURED_STEPS, LANDSCAPE_GX, 0,
                                            water ? feed_landscape_water : feed_landscape_sand, worst_out);
    two_core_scope_end(core);
    return per_step;
}

/* One landscape pour row: `build` at the app's rates, primed and timed by
 * landscape_scene_us_per_step(). Returns the mean. */
static int64_t
landscape_pour_row(void (*build)(sand_t*), bool water, const char* scene) {
    real_board_t b;
    sand_t* const real = real_board_open(&b, 29u);
    use_app_rates(real);
    build(real);

    int64_t worst = 0;
    const int64_t per_step = landscape_scene_us_per_step(real, water, &worst);

    log_step_and_worst(scene, per_step, worst);
    real_board_close(&b);
    free(real);
    return per_step;
}

/* Water poured into a settled sand bed, held the way the board is played
 * (build_landscape_bed_scene(), shared with
 * test_the_landscape_beds_sleep_against_the_landscape_floor). The dearest
 * of the three, and the pairing the palette puts first. */
static void
test_pouring_water_into_a_landscape_sand_bed_fits_in_the_frame_budget(void) {
    const int64_t per_step = landscape_pour_row(build_landscape_bed_scene, true, "landscape water onto a sand bed");
    perf_target("landscape water", per_step, LANDSCAPE_WATER_BUDGET_US, 27700);
}

/* The same pour onto a bed holding 65% of the board rather than 40%: a
 * shorter drop, far more settled mass for the skip to win or lose, and the
 * arena's other priced landscape depth. */
static void
test_pouring_water_into_a_deep_landscape_bed_fits_in_the_frame_budget(void) {
    const int64_t per_step =
        landscape_pour_row(build_landscape_deep_bed_scene, true, "landscape water onto a deep sand bed");
    perf_target("deep landscape water", per_step, LANDSCAPE_DEEP_WATER_BUDGET_US, 29310);
}

/* The liquid-free landscape row. Without it a geometry change that moved
 * the two rows above could not be told apart from one that moved the liquid
 * passes, since every other liquid-free scene in this file is portrait. */
static void
test_pouring_sand_onto_a_landscape_sand_bed_fits_in_the_frame_budget(void) {
    const int64_t per_step = landscape_pour_row(build_landscape_bed_scene, false, "landscape sand onto a sand bed");
    perf_target("landscape sand", per_step, LANDSCAPE_SAND_BUDGET_US, 7960);
}

/*
 * gfx_present() cost against real sand scenes
 *
 * Every frame-budget test above times sand_step() alone, with no drawing
 * involved - nothing has measured what gfx_present() actually costs against
 * the dirty pattern a real sand scene leaves (see suite_gfx.c for
 * synthetic-mark numbers only). These tests close that gap: build a real
 * scene, step it, reproduce app_sand.c's own marking policy, then time
 * gfx_present() on the result.
 */

/* REAL_W*REAL_CELL_PX == GFX_WIDTH and REAL_H*REAL_CELL_PX == GFX_HEIGHT -
 * REAL_W/REAL_H are the grid size at cell=2, the finest ("ULTRA") quality
 * tier in app_sand.c's qualities[] table, which is what the pixel math in
 * mirror_app_sand_marking()'s gfx_mark_dirty() calls has to agree with. */
#define REAL_CELL_PX 2

/* REPRODUCING, NOT CALLING: draw_dirty_row()/draw_one_row()/paint_row()
 * (app_sand.c) are static, inlined at their one call site - sharing a hot
 * per-call function across a translation-unit boundary previously cost a
 * measured 26% regression elsewhere.
 * Duplicates draw_dirty_row()'s policy instead (same row_runs calls, same
 * order), behind draw_dirty_rows()'s same dirty gate; paints no pixels, since
 * gfx_present()'s cost depends only on marked regions, never colour. */

/* Reconciles row cy's current runs against the ones recorded last frame,
 * writes the send ranges to send_x0/send_x1 and records the current runs in
 * their place. Returns the send count. */
static int
mirror_row_reconcile(const uint8_t* row, int w, int cy, uint16_t* row_x0, uint16_t* row_x1, uint8_t* row_n,
                     uint16_t* send_x0, uint16_t* send_x1) {
    uint16_t cur_x0[ROW_MAX_RUNS], cur_x1[ROW_MAX_RUNS];
    const int cur_n = row_runs_find_or_span(row, w, SAND_EMPTY, cur_x0, cur_x1);

    uint16_t* rprev_x0 = &row_x0[cy * ROW_MAX_RUNS];
    uint16_t* rprev_x1 = &row_x1[cy * ROW_MAX_RUNS];
    const int send_n = row_runs_reconcile(cur_x0, cur_x1, cur_n, rprev_x0, rprev_x1, row_n[cy], send_x0, send_x1);

    for (int i = 0; i < cur_n; i++) {
        rprev_x0[i] = cur_x0[i];
        rprev_x1[i] = cur_x1[i];
    }
    row_n[cy] = (uint8_t)cur_n;
    return send_n;
}

static void
mirror_app_sand_marking(const uint8_t* cells, int w, int h, uint8_t* dirty_rows, uint16_t* row_x0, uint16_t* row_x1,
                        uint8_t* row_n) {
    for (int cy = 0; cy < h; cy++) {
        if (!dirty_rows[cy]) {
            continue;
        }
        dirty_rows[cy] = 0;

        uint16_t send_x0[2 * ROW_MAX_RUNS], send_x1[2 * ROW_MAX_RUNS];
        const int send_n = mirror_row_reconcile(&cells[(size_t)cy * w], w, cy, row_x0, row_x1, row_n, send_x0, send_x1);

        for (int i = 0; i < send_n; i++) {
            gfx_mark_dirty(send_x0[i] * REAL_CELL_PX, cy * REAL_CELL_PX, (send_x1[i] - send_x0[i]) * REAL_CELL_PX,
                           REAL_CELL_PX);
        }
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
    panel_clock_pin(GFX_PANEL_CLOCK_FAST_HZ);
    for (int i = 0; i < settle_steps; i++) {
        sand_step(s, gx, gy, gz);
        mirror_app_sand_marking(cells, w, h, dirty_rows, row_x0, row_x1, row_n);
        gfx_present();
    }

    gfx_reset_strip_send_counts();

    int64_t sim_us = 0, mark_us = 0, present_us = 0;
    for (int i = 0; i < measured_steps; i++) {
        const int64_t t0 = timing_now_us();
        sand_step(s, gx, gy, gz);
        const int64_t t1 = timing_now_us();
        mirror_app_sand_marking(cells, w, h, dirty_rows, row_x0, row_x1, row_n);
        const int64_t t2 = timing_now_us();
        gfx_present();
        const int64_t t3 = timing_now_us();

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
    two_core_scope_end(core);

    return present_us / measured_steps;
}

/* The whole frame: the other rows time sand_step() without drawing, or the
 * bus alone. */
static void
log_frame_time(const char* frame, int64_t sim_us, int64_t mark_us, int64_t present_us) {
    ESP_LOGI("device_tests", "frame time, %s: sim %lld us/frame", frame, (long long)sim_us);
    ESP_LOGI("device_tests", "frame time, %s: mark %lld us/frame", frame, (long long)mark_us);
    ESP_LOGI("device_tests", "frame time, %s: present %lld us/frame", frame, (long long)present_us);
    ESP_LOGI("device_tests", "frame time, %s: total %lld us/frame", frame, (long long)(sim_us + mark_us + present_us));
}

#endif /* DEVICE_BUILD */

/* A present-cost scene's grid and sim, with what app_sand.c keeps per row to
 * mark a frame: the dirty flags the sim writes, the dirty column span
 * (dirty_x0/dirty_x1, NULL unless opened for the span mirror) and the runs
 * last sent. */
typedef struct {
    uint8_t* big;
    uint8_t* blocks;
    uint8_t* dirty;
    uint16_t* dirty_x0;
    uint16_t* dirty_x1;
    uint16_t* x0;
    uint16_t* x1;
    uint8_t* n;
    sand_t* sim;
} present_board_t;

/* Allocates the grid (and with `blocks` its block map), REAL_H rows of each
 * per-row buffer, then the sim, in that order; every row is seeded as last
 * sending the full width. The caller builds the scene into sim. */
static void
present_board_open(present_board_t* b, bool blocks, bool span) {
    b->blocks = NULL;
    if (blocks) {
        sand_test_grid_buffers_open(&b->big, &b->blocks, REAL_W, REAL_H);
    } else {
        b->big = malloc((size_t)REAL_W * REAL_H);
        TEST_ASSERT_NOT_NULL(b->big);
    }
    b->dirty = malloc(REAL_H);
    b->dirty_x0 = span ? malloc(REAL_H * sizeof(uint16_t)) : NULL;
    b->dirty_x1 = span ? malloc(REAL_H * sizeof(uint16_t)) : NULL;
    b->x0 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    b->x1 = malloc(REAL_H * ROW_MAX_RUNS * sizeof(uint16_t));
    b->n = malloc(REAL_H);
    TEST_ASSERT_NOT_NULL(b->dirty);
    TEST_ASSERT_TRUE(!span || ((b->dirty_x0 != NULL) && (b->dirty_x1 != NULL)));
    TEST_ASSERT_NOT_NULL(b->x0);
    TEST_ASSERT_NOT_NULL(b->x1);
    TEST_ASSERT_NOT_NULL(b->n);

    for (int i = 0; i < REAL_H; i++) {
        b->x0[i * ROW_MAX_RUNS] = 0;
        b->x1[i * ROW_MAX_RUNS] = (uint16_t)REAL_W;
        b->n[i] = 1;
    }
    b->sim = malloc(sizeof *b->sim);
    TEST_ASSERT_NOT_NULL(b->sim);
}

static void
present_board_close(present_board_t* b) {
    free(b->big);
    free(b->blocks);
    free(b->dirty);
    free(b->dirty_x0);
    free(b->dirty_x1);
    free(b->x0);
    free(b->x1);
    free(b->n);
    free(b->sim);
}

/* DENSE, CONTIGUOUS shape. Checkerboard exceeds ROW_MAX_RUNS (2).
 * row_runs_find() fails, row_runs_span_fallback() reports wide span.
 * gfx_present() handles. */
static void
build_falling_sand_present_scene(sand_t* real, uint8_t* big, uint8_t* dirty_rows) {
    build_full_size_step_scene(real, big);
    sand_track_dirty_rows(real, dirty_rows);
}

#define FALLING_SAND_PRESENT_FRAMES 20

/* Builds the falling-sand present scene and, on the board, presents it
 * FALLING_SAND_PRESENT_FRAMES frames after five settle frames: `bands` gets
 * the full-band, gathered and partial-band strip-sends, `phase_us` the sim,
 * mark and present means. */
static void
present_falling_sand_scene(int bands[3], int64_t phase_us[3]) {
    present_board_t b;
    present_board_open(&b, false, false);
    build_falling_sand_present_scene(b.sim, b.big, b.dirty);
    TEST_ASSERT_EQUAL_UINT8(1, b.dirty[(REAL_H / 2) - 1]);

#ifdef DEVICE_BUILD
    (void)run_present_against_scene(b.sim, b.big, REAL_W, REAL_H, b.dirty, b.x0, b.x1, b.n, 0, 1, 0, 5,
                                    FALLING_SAND_PRESENT_FRAMES, &bands[0], &bands[1], &bands[2], &phase_us[0],
                                    &phase_us[1], &phase_us[2]);
#else
    (void)bands;
    (void)phase_us;
#endif

    present_board_close(&b);
}

static void
test_present_cost_against_a_falling_sand_scene(void) {
    int bands[3] = {0};
    int64_t phase_us[3] = {0};
    present_falling_sand_scene(bands, phase_us);

#ifdef DEVICE_BUILD
    const int64_t mean_us = phase_us[2];
    ESP_LOGI("device_tests",
             "present cost, falling sand checkerboard, "
             "%dx%d: mean %lld us/frame over %d frames "
             "(%d full-band, %d gathered, %d partial-band "
             "strip-sends)",
             REAL_W, REAL_H, (long long)mean_us, FALLING_SAND_PRESENT_FRAMES, bands[0], bands[1], bands[2]);

    /* The scene owns its dirty-row buffer, which a bookkeeping fixture would
     * replace, leaving the present nothing to send and this row timing an
     * idle frame. */
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, bands[0] + bands[1] + bands[2],
                                         "the present sent no strip, so the row is not timing the bus");

    /* Present() is mostly irreducible bus time (docs/sand/Sand-Simulation.md,
     * "Performance discipline"); the only
     * movable thing is HOW MANY strips get sent, shown by the strip-send
     * counts beside the timing. */
    perf_target("present: falling sand", mean_us, 5050, 5810);
#endif
}

/* Present tests run the sim outside their own timer. Neither measures the
 * frame SUM, needed before justification. PRINTS, no frame budget argued yet.
 * Missing the real pixel writes - a LOWER BOUND only. */
static void
test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene(void) {
    int bands[3] = {0};
    int64_t phase_us[3] = {0};
    present_falling_sand_scene(bands, phase_us);

#ifdef DEVICE_BUILD
    const int64_t total_us = phase_us[0] + phase_us[1] + phase_us[2];
    const int present_pct = total_us > 0 ? (int)((phase_us[2] * 100) / total_us) : 0;

    log_frame_time("falling sand checkerboard", phase_us[0], phase_us[1], phase_us[2]);
    ESP_LOGI("device_tests",
             "frame time, falling sand checkerboard: present is %d%% of "
             "the total",
             present_pct);
#endif
}

#ifdef DEVICE_BUILD
/* One present-cost row: a REAL_W x REAL_H scene from `build` at the app's
 * rates, settled settle_steps, then presented measured_steps frames. Logs
 * under `frame` and `scene`; returns the mean present cost per frame. */
static int64_t
present_cost_of_scene(uint32_t seed, void (*build)(sand_t* s), int settle_steps, int measured_steps, const char* frame,
                      const char* scene) {
    present_board_t b;
    present_board_open(&b, true, false);
    sand_t* const real = b.sim;
    sand_init(real, b.big, REAL_W, REAL_H, seed);
    sand_enable_sleeping(real, b.blocks);
    use_app_rates(real);
    sand_track_dirty_rows(real, b.dirty);

    build(real);

    int full_bands = 0, gathered = 0, partial_bands = 0;
    int64_t sim_us = 0, mark_us = 0, present_us = 0;
    const int64_t mean_us = run_present_against_scene(real, b.big, REAL_W, REAL_H, b.dirty, b.x0, b.x1, b.n, 0, 1000, 0,
                                                      settle_steps, measured_steps, &full_bands, &gathered,
                                                      &partial_bands, &sim_us, &mark_us, &present_us);

    log_frame_time(frame, sim_us, mark_us, present_us);
    ESP_LOGI("device_tests",
             "present cost, %s, %dx%d: mean %lld us/frame over %d frames (%d full-band, %d gathered, %d "
             "partial-band strip-sends)",
             scene, REAL_W, REAL_H, (long long)mean_us, measured_steps, full_bands, gathered, partial_bands);

    present_board_close(&b);
    return mean_us;
}

static void
test_present_cost_against_the_lava_stress_scene(void) {
    const int64_t mean_us =
        present_cost_of_scene(37u, build_lava_stress_scene, 30, 20, "lava stress", "lava stress scene");
    perf_guard("present: lava stress", mean_us, 8350);
}

static void
test_present_cost_against_the_thermal_shock_scene(void) {
    const int64_t mean_us =
        present_cost_of_scene(41u, build_thermal_shock_scene, 0, 10, "thermal shock", "thermal shock lattice");

    /* 70/70 full strip-sends and zero gathered is correct, not a target:
     * this lattice dirties every strip every frame, so an oracle sends the
     * same 164,864 pixels. Watch pixels sent. A failure likely means the
     * scene dirties MORE pixels, not a slower present. */
    perf_guard("present: thermal shock", mean_us, 11320);
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

        uint16_t send_x0[2 * ROW_MAX_RUNS], send_x1[2 * ROW_MAX_RUNS];
        const int send_n = mirror_row_reconcile(&cells[(size_t)cy * w], w, cy, row_x0, row_x1, row_n, send_x0, send_x1);

        mirror_row_send_dirty(cy, wx0, wx1, send_x0, send_x1, send_n, pixels_sent_accum);
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
    panel_clock_pin(GFX_PANEL_CLOCK_FAST_HZ);
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
        const int64_t t0 = timing_now_us();
        gfx_present();
        present_us += timing_now_us() - t0;
    }

    gfx_get_strip_send_counts(full_bands, gathered, partial_bands);

    if (pixels_sent_out != NULL) {
        *pixels_sent_out = pixels_sent / measured_steps;
    }
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
    use_app_rates(real);
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
    sand_fill_box(real, 0, 0, POOL_UNEVEN_DEEP_X1, REAL_H / 2, CELL_MAKE(MAT_WATER, MASS_MAX));
    sand_fill_box(real, 0, REAL_H / 2, POOL_UNEVEN_SHALLOW_X1, REAL_H, CELL_MAKE(MAT_WATER, MASS_MAX));
}

#define PRESENT_COST_MEASURED_STEPS 20

/* A freshly built copy of `build` under the row-only mirror, or with
 * `span` the column-span one: `bands` gets the strip-sends, *pixels_sent the
 * span mirror's pixels per frame. */
static int64_t
measure_present_cost(void (*build)(sand_t*, uint8_t*, uint8_t*), int gx, int gy, bool span, int bands[3],
                     int64_t* pixels_sent) {
    present_board_t b;
    present_board_open(&b, true, span);
    build(b.sim, b.big, b.blocks);
    sand_track_dirty_rows(b.sim, b.dirty);

    const int64_t us =
        span
            ? run_present_against_scene_span(b.sim, b.big, REAL_W, REAL_H, b.dirty, b.dirty_x0, b.dirty_x1, b.x0, b.x1,
                                             b.n, gx, gy, 0, 20, PRESENT_COST_MEASURED_STEPS, &bands[0], &bands[1],
                                             &bands[2], pixels_sent)
            : run_present_against_scene(b.sim, b.big, REAL_W, REAL_H, b.dirty, b.x0, b.x1, b.n, gx, gy, 0, 20,
                                        PRESENT_COST_MEASURED_STEPS, &bands[0], &bands[1], &bands[2], NULL, NULL, NULL);

    present_board_close(&b);
    return us;
}

/* Runs `build` under both mirrors, back to back, so the before/after numbers
 * come from one run rather than two captures that could drift apart. */
static void
report_span_vs_row_present_cost(const char* scene_name, void (*build)(sand_t*, uint8_t*, uint8_t*), int gx, int gy) {
    int row[3] = {0};
    const int64_t row_us = measure_present_cost(build, gx, gy, false, row, NULL);

    int span[3] = {0};
    int64_t pixels_sent = 0;
    const int64_t span_us = measure_present_cost(build, gx, gy, true, span, &pixels_sent);

    ESP_LOGI("device_tests",
             "present cost, %s, %dx%d: ROW-only %lld us/frame (%d full, %d "
             "gathered, %d partial) vs COLUMN-span %lld us/frame (%d full, "
             "%d gathered, %d partial, %lld px/frame)",
             scene_name, REAL_W, REAL_H, (long long)row_us, row[0], row[1], row[2], (long long)span_us, span[0],
             span[1], span[2], (long long)pixels_sent);
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

    /* HEAP, not static file scope, see drop_impulse_buf's own comment
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
     * free() after one never runs, see drop_impulse_buf's own comment
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

    /* HEAP, not static file scope, see drop_impulse_buf's own comment
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
     * free() after one never runs, see drop_impulse_buf's own comment
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

/* water slope: reported gravity-flip drop over a covered slope */

#ifdef DEVICE_BUILD
static int
water_slope_liquid_near_blocks(const sand_t* s) {
    int n = 0;
    for (int by = 0; by < s->block_rows; by++) {
        for (int bx = 0; bx < s->block_cols; bx++) {
            if ((s->block_state[((size_t)by * (size_t)s->block_cols) + (size_t)bx] & BLOCK_LIQUID_NEAR) != 0) {
                n++;
            }
        }
    }
    return n;
}

static long
water_slope_water_mass(const sand_t* s) {
    return mass_of(s, s->w, s->h, MAT_WATER);
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
log_pass_split(const char* name, int steps, int impulse_max, const pass_split_t* split, unsigned cap_hits) {
    const int64_t* const totals = split->totals;
    const int64_t* const peak = split->peak;
    const int peak_impulses = split->peak_impulses;
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
    uint8_t* big;
    uint8_t* blocks;
    sand_t* const real = sand_test_grid_open(&big, &blocks, REAL_W, REAL_H, 29u);
    sand_set_soak(real, SAND_SOAK_PER_MATERIAL);
    build_landscape_bed_scene(real);

    for (int i = 0; i < SUBMERGED_PILE_POUR_STEPS; i++) {
        landscape_water_pour(real, i);
        sand_step(real, LANDSCAPE_GX, 0, 0);
    }
    const long mass_before = water_slope_water_mass(real);

    int settled_at = -1;
    for (int i = 0; i < SUBMERGED_PILE_FULL_SETTLE_STEPS; i++) {
        const unsigned d0 = sand_reactions_cells_dispatched;
        const unsigned m0 = sand_liquid_moves;
        const unsigned p0 = sand_liquid_crossflow_probes;
        const unsigned sw0 = sand_liquid_sweep_moves;
        sand_step(real, LANDSCAPE_GX, 0, 0);
        const int awake = count_awake_blocks(real);
        if (i % 100 == 0 || i == SUBMERGED_PILE_FULL_SETTLE_STEPS - 1) {
            water_slope_log_step("settle", i, real, sand_reactions_cells_dispatched - d0, sand_liquid_moves - m0,
                                 sand_liquid_crossflow_probes - p0, sand_liquid_sweep_moves - sw0);
        }
        if (awake == 0 && settled_at < 0) {
            settled_at = i;
        }
    }
    const long mass_after = water_slope_water_mass(real);
    const int awake_at_end = count_awake_blocks(real);

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
    free(real);
}

/* Task 1a: water poured continuously at the slope's high corner until it
 * covers the slope and runs down. Logs the pass split averaged over the
 * pour, then asserts only that real work happened - this scene exists to
 * characterise a cost, not to gate one yet. */
static void
test_water_slope_pouring_water_logs_the_pass_split(void) {
    uint8_t* big;
    uint8_t* blocks;
    sand_t* const real = sand_test_grid_open(&big, &blocks, REAL_W, REAL_H, 41u);
    build_water_slope_scene(real);

    int64_t liquid_total = 0, reactions_total = 0, sweep_total = 0;
    const int steps = WATER_SLOPE_COVER_STEPS;
    for (int i = 0; i < steps; i++) {
        water_slope_water_pour(real, i);
        sand_step(real, LANDSCAPE_GX, 0, 0);
        sweep_total += real->pass_us.sweep_us;
        liquid_total += real->pass_us.liquid_us;
        reactions_total += real->pass_us.reactions_us;
    }
    const long mass = water_slope_water_mass(real);
    const int liq_near = water_slope_liquid_near_blocks(real);

    free(big);
    free(blocks);

    ESP_LOGI("device_tests", "water slope pour, %d steps: mean sweep=%d liq=%d react=%d us, water_mass=%ld liqnear=%d",
             steps, (int)(sweep_total / steps), (int)(liquid_total / steps), (int)(reactions_total / steps), mass,
             liq_near);

    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)mass, "the pour must actually place water on the board");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, liq_near,
                                         "the pour must reach BLOCK_LIQUID_NEAR blocks, or the "
                                         "reactions soak-only skip has nothing to walk");
    free(real);
}

/* Task 1c: the three controls beside the covered slope, one line each - a
 * dry slope (no liquid pass at all), water over a flat pile (same pour
 * mechanism, no diagonal), and water over stone (the same diagonal, nothing
 * wettable). Compares them against the covered slope on the counters that
 * matter: reactions dispatch and cross-flow moves/probes. */
static void
test_water_slope_controls_log_the_pass_split(void) {
    uint8_t* big;
    uint8_t* blocks;
    sand_test_grid_buffers_open(&big, &blocks, REAL_W, REAL_H);

    sand_t* const real = malloc(sizeof *real);
    TEST_ASSERT_NOT_NULL(real);
    long masses[4] = {0};
    const char* names[4] = {"dry", "slope", "flat", "stone"};
    void (*const builds[4])(sand_t*) = {build_water_slope_scene, build_water_slope_covered_scene,
                                        build_water_slope_flat_covered_scene, build_water_slope_stone_covered_scene};

    for (int i = 0; i < 4; i++) {
        sand_init(real, big, REAL_W, REAL_H, 41u);
        sand_enable_sleeping(real, blocks);
        builds[i](real);
        if (i == 0) {
            run_steps(real, 60, LANDSCAPE_GX, 0);
        }
        water_slope_step_and_log(real, LANDSCAPE_GX, 0, names[i], 0);
        masses[i] = water_slope_water_mass(real);
    }

    free(big);
    free(blocks);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)masses[0], "the dry control must hold no water at all");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)masses[1], "the covered slope must hold water");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)masses[2], "the flat-pile control must hold water");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, (int)masses[3], "the stone control must hold water");
    free(real);
}

/* Task 1b, and the maintainer's own refinement: settle the covered slope in
 * landscape, tilt right into portrait, hold, tilt back. One line per step
 * through the whole sequence - the flip itself, not only its settled ends,
 * is what the report says is worst. */
static void
test_water_slope_gravity_flip_logs_a_per_step_table(void) {
    uint8_t* big;
    uint8_t* blocks;
    sand_t* const real = sand_test_grid_open(&big, &blocks, REAL_W, REAL_H, 41u);
    build_water_slope_covered_scene(real);

    const long mass_before = water_slope_water_mass(real);

    for (int i = 0; i < WATER_SLOPE_FLIP_SETTLE_STEPS; i++) {
        water_slope_step_and_log(real, LANDSCAPE_GX, 0, "settle", i);
    }
    for (int i = 1; i <= WATER_SLOPE_FLIP_TURN_STEPS; i++) {
        const int gx = LANDSCAPE_GX - ((LANDSCAPE_GX * i) / WATER_SLOPE_FLIP_TURN_STEPS);
        const int gy = (WATER_SLOPE_PORTRAIT_GY * i) / WATER_SLOPE_FLIP_TURN_STEPS;
        water_slope_step_and_log(real, gx, gy, "to_port", i);
    }
    for (int i = 0; i < WATER_SLOPE_FLIP_HOLD_STEPS; i++) {
        water_slope_step_and_log(real, WATER_SLOPE_PORTRAIT_GX, WATER_SLOPE_PORTRAIT_GY, "hold", i);
    }
    for (int i = 1; i <= WATER_SLOPE_FLIP_TURN_STEPS; i++) {
        const int gx = (LANDSCAPE_GX * i) / WATER_SLOPE_FLIP_TURN_STEPS;
        const int gy = WATER_SLOPE_PORTRAIT_GY - ((WATER_SLOPE_PORTRAIT_GY * i) / WATER_SLOPE_FLIP_TURN_STEPS);
        water_slope_step_and_log(real, gx, gy, "to_land", i);
    }
    for (int i = 0; i < WATER_SLOPE_FLIP_HOLD_STEPS; i++) {
        water_slope_step_and_log(real, LANDSCAPE_GX, 0, "recover", i);
    }

    const long mass_after = water_slope_water_mass(real);

    free(big);
    free(blocks);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE((int)mass_after, (int)mass_before,
                                             "a tilt right into portrait and back must never create water - "
                                             "soaking may only ever spend it");
    free(real);
}

/* The mid-task correction: a scene seeded directly from a device screenshot
 * of the reported drop, gravity swept between the two tilt vectors the same
 * capture pair recorded - a partly diagonal change, not an axis-aligned
 * flip. */
static void
test_water_slope_captured_scene_diagonal_flip_logs_a_per_step_table(void) {
    uint8_t* big;
    uint8_t* blocks;
    sand_t* const real = sand_test_grid_open(&big, &blocks, REAL_W, REAL_H, 41u);
    build_captured_water_slope_scene(real);

    const long mass_before = water_slope_water_mass(real);

    for (int i = 1; i <= WATER_SLOPE_CAPTURED_SWEEP_STEPS; i++) {
        const int gx = WATER_SLOPE_CAPTURED_TILT1_GX
                       + (((WATER_SLOPE_CAPTURED_TILT2_GX - WATER_SLOPE_CAPTURED_TILT1_GX) * i)
                          / WATER_SLOPE_CAPTURED_SWEEP_STEPS);
        const int gy = WATER_SLOPE_CAPTURED_TILT1_GY
                       + (((WATER_SLOPE_CAPTURED_TILT2_GY - WATER_SLOPE_CAPTURED_TILT1_GY) * i)
                          / WATER_SLOPE_CAPTURED_SWEEP_STEPS);
        water_slope_step_and_log(real, gx, gy, "captured", i);
    }

    const long mass_after = water_slope_water_mass(real);

    free(big);
    free(blocks);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE((int)mass_after, (int)mass_before,
                                             "the captured diagonal gravity change must never create water - "
                                             "soaking may only ever spend it");
    free(real);
}

/*
 * the panel clock and heal against a real pour
 *
 * Drives the real app_sand.c, touch pour and all, and times gfx_present()
 * three ways: 40 MHz, 80 MHz, and 80 MHz with sand's heal policy. The heal
 * is only worth having if the third stays clearly under the first.
 */

#include "app/app.h"

extern app_t app_sand;
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
    const int64_t t0 = timing_now_us();
    gfx_present();
    *present_us += timing_now_us() - t0;
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
    frame_watch_restart();
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
    frame_watch_restart();
    sand_app_restore_colour_mode_for_test(previous_mode);
    gfx_heal_restore_defaults();
}

static void
test_present_cost_at_40_mhz_80_mhz_and_80_mhz_with_heal_on_a_real_pour(void) {
    static const char* const brush_names[] = {"sand", "water"};
    static const char* const row_names[CLOCK_ROW_COUNT] = {"40", "80", "80+heal"};
    const two_core_scope_t core = two_core_scope_begin(true);
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
    two_core_scope_end(core);

    /* Asserted only after the two-core scope ends: a failing assert longjmps
     * out of it. */
    TEST_ASSERT_FALSE_MESSAGE(unhealed_row_sent_heal, "only the heal row may send heal strips");
}

#endif /* DEVICE_BUILD */

/* suite */

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
    RUN_TEST(test_present_cost_against_a_falling_sand_scene);
    RUN_TEST(test_a_real_frame_is_sim_plus_present_on_a_falling_sand_scene);
    RUN_TEST(test_acid_bubbles_do_not_favour_one_wall);
    RUN_TEST(test_acid_bubbles_still_fire_once_the_block_is_asleep);
    RUN_TEST(test_the_soak_only_skip_dispatches_far_fewer_cells_than_a_full_walk);
    RUN_TEST(test_the_soak_only_skip_matches_the_full_walks_grid_exactly);
    RUN_TEST(test_the_soak_only_skip_hash_survives_ambient_two_core_state);
    RUN_TEST(test_every_swept_chunk_layout_is_one_the_planner_takes);
    RUN_TEST(test_the_sweep_measures_both_cuts_every_quality_ships);
    RUN_TEST(test_the_sweeps_gas_scenes_put_work_in_both_gas_passes);

#ifdef DEVICE_BUILD
    RUN_TEST(test_a_frame_budget_board_really_reaches_the_split_path);
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
    RUN_TEST(test_a_packed_landscape_screen_of_gas_fits_in_the_frame_budget);
    RUN_TEST(test_a_full_landscape_screen_of_fire_fits_in_the_frame_budget);
    RUN_TEST(test_fire_cascading_through_a_full_landscape_screen_of_gas_fits_in_the_frame_budget);
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

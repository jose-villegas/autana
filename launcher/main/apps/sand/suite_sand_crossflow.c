#include <stdio.h>
#include <stdlib.h>

#include "sand_priv.h"
#include "suite_sand_common.h"
#include "suites.h"
#include "unity.h"

#define CF_W 19
#define CF_H 160

typedef struct {
    sand_t s;
    uint8_t cells[CF_W * CF_H];
    uint8_t blocks[((CF_W + SAND_BLOCK_W - 1) / SAND_BLOCK_W) * (CF_H / SAND_BLOCK_H)];
    uint8_t dirty[CF_H];
    uint16_t x0[CF_H], x1[CF_H];
    uint8_t stamps[CF_H * ((CF_W + 7) / 8)];
    void* scratch;
} crossflow_fixture_t;

static void
crossflow_free(crossflow_fixture_t* f) {
    free(f->scratch);
    free(f);
}

static crossflow_fixture_t*
crossflow_fixture(void) {
    crossflow_fixture_t* f = malloc(sizeof *f);
    TEST_ASSERT_NOT_NULL(f);
    sand_init(&f->s, f->cells, CF_W, CF_H, 42u);
    sand_enable_sleeping(&f->s, f->blocks);
    sand_track_dirty_rows(&f->s, f->dirty);
    sand_track_dirty_cols(&f->s, f->x0, f->x1);
    TEST_ASSERT_EQUAL_UINT((unsigned)sizeof f->stamps, (unsigned)sand_step_stamp_bytes(CF_W, CF_H));
    sand_enable_step_stamps(&f->s, f->stamps);
    f->scratch = lane_scratch_open(&f->s);
    for (int y = 0; y < CF_H; y++) {
        for (int x = 0; x < CF_W; x++) {
            sand_set(&f->s, x, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        }
    }
    f->s.step_phase = 1;
    f->s.liquid_flip = true;
    return f;
}

enum { SORT_W = 8, SORT_H = 8 };

static void
sort_fixture(sand_t* s, uint8_t cells[SORT_W * SORT_H]) {
    sand_init(s, cells, SORT_W, SORT_H, 42u);
    for (int y = 0; y < SORT_H; y++) {
        for (int x = 0; x < SORT_W; x++) {
            const bool border = x == 0 || x == SORT_W - 1 || y == 0 || y == SORT_H - 1;
            sand_set(s, x, y, border ? STONE : OIL);
        }
    }
    s->step_phase = 2;
}

static void
sort_liquids(sand_t* s, int dx, int dy) {
    const xflow_t flow = {0};
    sand_step_liquids(s, &flow, dx, dy);
}

static void
test_liquid_density_sort_moves_one_landscape_cell(void) {
    for (int dx = -1; dx <= 1; dx += 2) {
        uint8_t cells[SORT_W * SORT_H];
        sand_t s;
        sort_fixture(&s, cells);

        const int x = dx > 0 ? 1 : SORT_W - 2;
        sand_set(&s, x, SORT_H / 2, WATER);
        sort_liquids(&s, dx, 0);

        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(sand_at(&s, x + dx, SORT_H / 2)),
                                      "a denser liquid may sink only one landscape cell per sort pass");
    }
}

static void
test_liquid_density_sort_swaps_a_landscape_boundary_in_every_row(void) {
    uint8_t cells[SORT_W * SORT_H];
    sand_t s;
    sort_fixture(&s, cells);

    for (int y = 1; y < SORT_H - 1; y++) {
        sand_set(&s, 1, y, WATER);
    }
    sort_liquids(&s, 1, 0);

    for (int y = 1; y < SORT_H - 1; y++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(sand_at(&s, 2, y)),
                                      "every row along a landscape density boundary must swap");
    }
}

static void
test_liquid_density_sort_moves_one_cell_along_a_diagonal(void) {
    uint8_t cells[SORT_W * SORT_H];
    sand_t s;
    sort_fixture(&s, cells);
    sand_set(&s, 1, 1, WATER);

    sort_liquids(&s, 1, 1);

    TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(sand_at(&s, 2, 2)),
                                  "a diagonal density sort must stop the displaced liquid after one ray cell");
}

static void
test_liquid_density_sort_keeps_the_portrait_rate(void) {
    uint8_t cells[SORT_W * SORT_H];
    sand_t s;
    sort_fixture(&s, cells);

    for (int x = 1; x < SORT_W - 1; x++) {
        sand_set(&s, x, 1, WATER);
    }
    sort_liquids(&s, 0, 1);

    for (int x = 1; x < SORT_W - 1; x++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(MAT_WATER, CELL_MATERIAL(sand_at(&s, x, 2)),
                                      "portrait density sorting must retain its one-cell rate");
    }
}

static void
test_split_crossflow_uses_hashed_viscosity(void) {
    crossflow_fixture_t* f = crossflow_fixture();
    for (int y = 2; y < CF_H - 2; y += 4) {
        sand_set(&f->s, 9, y, CELL_MAKE(MAT_OIL, 15));
        sand_set(&f->s, 9, y + 1, CELL_MAKE(MAT_OIL, 3));
    }
    memset(f->blocks, BLOCK_HAS_LIQUID, sizeof f->blocks);
    f->s.may_have_viscous_liquid = true;
    f->s.mobility = 128;
    const rng_t before = f->s.rng;
    const xflow_t flow = {.ax = {0, 1}, .dg = {0, 1}};
    sand_set_two_core_step(true);
    sand_step_liquids(&f->s, &flow, 1, 0);
    sand_set_two_core_step(false);
    const rng_t after = f->s.rng;
    crossflow_free(f);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&before, &after, sizeof before,
                                     "split cross-flow must leave the sequential RNG untouched");
}

static void
crossflow_step(crossflow_fixture_t* f, int px, int py, bool split) {
    const xflow_t flow = {.ax = {px, py}, .dg = {px, py}, .q_q8 = px != 0 && py != 0 ? 256 : 0};
    memset(f->blocks, BLOCK_HAS_LIQUID, sizeof f->blocks);
    memset(f->dirty, 0, sizeof f->dirty);
    for (int y = 0; y < CF_H; y++) {
        f->x0[y] = CF_W;
        f->x1[y] = 0;
    }
    f->s.last_load_dx = -py;
    f->s.last_load_dy = px;
    sand_set_two_core_step(split);
    sand_step_liquids(&f->s, &flow, -py, px);
    sand_set_two_core_step(false);
}

static unsigned
crossflow_water_mass(const uint8_t* cells) {
    unsigned mass = 0;
    for (int i = 0; i < CF_W * CF_H; i++) {
        if (CELL_MATERIAL(cells[i]) == MAT_WATER) {
            mass += CELL_VARIANT(cells[i]);
        }
    }
    return mass;
}

/* Every cell a chunk pass changed has to come back through the merge: into
 * the row's repaint span, and out of its block's settled bits. A private
 * copy per worker is the only reason those writes are safe at all, so a
 * merge that drops one is exactly the bug this looks for. */
static void
assert_every_change_was_merged(const crossflow_fixture_t* f, const uint8_t* before) {
    for (int y = 0; y < CF_H; y++) {
        for (int x = 0; x < CF_W; x++) {
            if (f->cells[y * CF_W + x] == before[y * CF_W + x]) {
                continue;
            }
            TEST_ASSERT_TRUE_MESSAGE(f->dirty[y], "a changed row must be repainted");
            TEST_ASSERT_TRUE_MESSAGE(f->x0[y] <= x && x < f->x1[y], "a changed column must be inside the row's span");
            TEST_ASSERT_FALSE_MESSAGE(sand_block_settled(&f->s, x / SAND_BLOCK_W, y / SAND_BLOCK_H),
                                      "a changed cell's block must be awake");
        }
    }
}

/* One (px,py,phase,trial) case: an isolated water transfer near a chunk
 * boundary, stepped serially and split. Mass that lands in a chunk whose own
 * pass is still to come must not be forwarded again there, so the split
 * board matches serial cell for cell, repaint for repaint, wake for wake. */
static void
run_crossflow_seam_trial(int px, int py, int phase, int trial) {
    const int delta = trial / 2 - SAND_LIQUID_SIGHT - 1;
    const int distance = (trial & 1) ? SAND_LIQUID_SIGHT : 1;
    crossflow_fixture_t* serial = crossflow_fixture();
    crossflow_fixture_t* split = crossflow_fixture();
    uint8_t* before = malloc(CF_W * CF_H);
    TEST_ASSERT_NOT_NULL(before);

    const int y = 2 * sand_chunk_side(&split->s) + delta;
    crossflow_fixture_t* fixtures[] = {serial, split};
    for (int i = 0; i < 2; i++) {
        sand_t* s = &fixtures[i]->s;
        s->step_phase = (uint16_t)phase;
        sand_set(s, 9, y, CELL_MAKE(MAT_WATER, 15));
        for (int k = 1; k < distance; k++) {
            sand_set(s, 9 + k * px, y + k * py, CELL_MAKE(MAT_WATER, 14));
            sand_set(s, 9 + k * px - py, y + k * py + px, CELL_EMPTY);
        }
        sand_set(s, 9 + distance * px, y + distance * py, CELL_EMPTY);
        sand_set(s, 9 + (distance + 1) * px, y + (distance + 1) * py, CELL_EMPTY);
    }
    memcpy(before, split->cells, CF_W * CF_H);

    crossflow_step(serial, px, py, false);
    crossflow_step(split, px, py, true);

    const unsigned serial_mass = crossflow_water_mass(serial->cells);
    const unsigned split_mass = crossflow_water_mass(split->cells);
    const bool equal = memcmp(serial->cells, split->cells, sizeof serial->cells) == 0;
    const bool dirty_equal = memcmp(serial->dirty, split->dirty, sizeof serial->dirty) == 0
                             && memcmp(serial->x0, split->x0, sizeof serial->x0) == 0
                             && memcmp(serial->x1, split->x1, sizeof serial->x1) == 0;
    const bool blocks_equal = memcmp(serial->blocks, split->blocks, sizeof serial->blocks) == 0;
    assert_every_change_was_merged(split, before);

    free(before);
    crossflow_free(serial);
    crossflow_free(split);
    char why[160];
    snprintf(why, sizeof why, "ray %d,%d phase %d trial %d: an isolated transfer may move only once across a seam", px,
             py, phase, trial);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(serial_mass, split_mass, "a split transfer may move mass, never make or lose it");
    TEST_ASSERT_TRUE_MESSAGE(equal, why);
    TEST_ASSERT_TRUE_MESSAGE(dirty_equal, "split must merge every depth repaint and dirty span");
    TEST_ASSERT_TRUE_MESSAGE(blocks_equal, "split must merge every wake flag");
}

static void
test_crossflow_seam_transfer_matches_serial_in_eight_directions(void) {
    for (int px = -1; px <= 1; px++) {
        for (int py = -1; py <= 1; py++) {
            if (px == 0 && py == 0) {
                continue;
            }
            for (int phase = 0; phase < 2; phase++) {
                for (int trial = 0; trial < 2 * (2 * SAND_LIQUID_SIGHT + 3); trial++) {
                    run_crossflow_seam_trial(px, py, phase, trial);
                }
            }
        }
    }
}

static crossflow_fixture_t*
crossflow_pool(void) {
    crossflow_fixture_t* f = crossflow_fixture();
    for (int y = 1; y < CF_H - 1; y++) {
        for (int x = 1; x < CF_W - 1; x++) {
            sand_set(&f->s, x, y, CELL_MAKE(MAT_WATER, 1 + (x * 7 + y * 11) % 15));
        }
    }
    return f;
}

static void
test_crossflow_pool_conserves_mass_and_is_deterministic(void) {
    crossflow_fixture_t* a = crossflow_pool();
    crossflow_fixture_t* b = crossflow_pool();
    crossflow_fixture_t* serial = crossflow_pool();
    const unsigned mass = crossflow_water_mass(a->cells);
    static const int rays[][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}, {1, 1}, {-1, -1}, {-1, 1}, {1, -1}};
    for (int step = 0; step < 64; step++) {
        const int* ray = rays[step % 8];
        a->s.step_phase = b->s.step_phase = serial->s.step_phase = (uint16_t)step;
        crossflow_step(a, ray[0], ray[1], true);
        crossflow_step(b, ray[0], ray[1], true);
        crossflow_step(serial, ray[0], ray[1], false);
        TEST_ASSERT_EQUAL_UINT(mass, crossflow_water_mass(a->cells));
        TEST_ASSERT_EQUAL_UINT(mass, crossflow_water_mass(serial->cells));
        TEST_ASSERT_EQUAL_MEMORY(a->cells, b->cells, sizeof a->cells);
        TEST_ASSERT_EQUAL_MEMORY(a->blocks, b->blocks, sizeof a->blocks);
    }
    crossflow_free(a);
    crossflow_free(b);
    crossflow_free(serial);
}

static void
test_crossflow_uniform_pool_has_no_chunk_seams(void) {
    crossflow_fixture_t* f = crossflow_fixture();
    for (int y = 0; y < CF_H; y++) {
        sand_set(&f->s, 9, y, CELL_MAKE(MAT_WATER, 1 + y % 15));
    }
    const unsigned mass = crossflow_water_mass(f->cells);
    for (int step = 0; step < 600; step++) {
        f->s.step_phase = (uint16_t)step;
        crossflow_step(f, 0, 1, true);
    }
    unsigned max_jump = 0;
    for (int y = 1; y < CF_H; y++) {
        const int level = CELL_VARIANT(f->cells[y * CF_W + 9]);
        const int above = CELL_VARIANT(f->cells[(y - 1) * CF_W + 9]);
        const unsigned jump = (unsigned)abs(level - above);
        if (jump > max_jump) {
            max_jump = jump;
        }
    }
    const unsigned after = crossflow_water_mass(f->cells);
    crossflow_free(f);
    TEST_ASSERT_EQUAL_UINT(mass, after);
    TEST_ASSERT_LESS_OR_EQUAL_UINT_MESSAGE(1, max_jump,
                                           "neighboring rows, including seams, must level to within one mass unit");
}

void
run_sand_crossflow_suite(void) {
    RUN_TEST(test_liquid_density_sort_moves_one_landscape_cell);
    RUN_TEST(test_liquid_density_sort_swaps_a_landscape_boundary_in_every_row);
    RUN_TEST(test_liquid_density_sort_moves_one_cell_along_a_diagonal);
    RUN_TEST(test_liquid_density_sort_keeps_the_portrait_rate);
    RUN_TEST(test_split_crossflow_uses_hashed_viscosity);
    RUN_TEST(test_crossflow_seam_transfer_matches_serial_in_eight_directions);
    RUN_TEST(test_crossflow_pool_conserves_mass_and_is_deterministic);
    RUN_TEST(test_crossflow_uniform_pool_has_no_chunk_seams);
}

SUITE_REGISTER(run_sand_crossflow_suite);

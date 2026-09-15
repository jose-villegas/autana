#include <stdlib.h>

#include "sand_priv.h"
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
} crossflow_fixture_t;

static crossflow_fixture_t*
crossflow_fixture(void) {
    crossflow_fixture_t* f = malloc(sizeof *f);
    TEST_ASSERT_NOT_NULL(f);
    sand_init(&f->s, f->cells, CF_W, CF_H, 42u);
    sand_enable_sleeping(&f->s, f->blocks);
    sand_track_dirty_rows(&f->s, f->dirty);
    sand_track_dirty_cols(&f->s, f->x0, f->x1);
    for (int y = 0; y < CF_H; y++) {
        for (int x = 0; x < CF_W; x++) {
            sand_set(&f->s, x, y, CELL_MAKE(MAT_STONE, SAND_AMBIENT_HEAT));
        }
    }
    f->s.step_phase = 1;
    f->s.liquid_flip = true;
    return f;
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
    free(f);
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

static void
test_crossflow_seam_transfer_matches_serial_in_eight_directions(void) {
    for (int px = -1; px <= 1; px++) {
        for (int py = -1; py <= 1; py++) {
            if (px == 0 && py == 0) {
                continue;
            }
            for (int phase = 0; phase < 2; phase++) {
                const int offset = phase ? SAND_BLOCK_H / 2 : 0;
                for (int trial = 0; trial < 2 * (2 * SAND_LIQUID_SIGHT + 3); trial++) {
                    const int delta = trial / 2 - SAND_LIQUID_SIGHT - 1;
                    const int distance = (trial & 1) ? SAND_LIQUID_SIGHT : 1;
                    crossflow_fixture_t* serial = crossflow_fixture();
                    crossflow_fixture_t* split = crossflow_fixture();
                    const int y = 2 * SAND_BLOCK_H + offset + delta;
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
                    crossflow_step(serial, px, py, false);
                    crossflow_step(split, px, py, true);
                    const bool equal = memcmp(serial->cells, split->cells, sizeof serial->cells) == 0;
                    const bool dirty_equal = memcmp(serial->dirty, split->dirty, sizeof serial->dirty) == 0
                                             && memcmp(serial->x0, split->x0, sizeof serial->x0) == 0
                                             && memcmp(serial->x1, split->x1, sizeof serial->x1) == 0;
                    const bool blocks_equal = memcmp(serial->blocks, split->blocks, sizeof serial->blocks) == 0;
                    free(serial);
                    free(split);
                    TEST_ASSERT_TRUE_MESSAGE(equal, "an isolated transfer may move only once across a seam");
                    TEST_ASSERT_TRUE_MESSAGE(dirty_equal, "split must merge every depth repaint and dirty span");
                    TEST_ASSERT_TRUE_MESSAGE(blocks_equal, "split must merge every wake flag");
                }
            }
        }
    }
}

static unsigned
crossflow_mass(const crossflow_fixture_t* f) {
    unsigned mass = 0;
    for (int i = 0; i < CF_W * CF_H; i++) {
        if (CELL_MATERIAL(f->cells[i]) == MAT_WATER) {
            mass += CELL_VARIANT(f->cells[i]);
        }
    }
    return mass;
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
    const unsigned mass = crossflow_mass(a);
    static const int rays[][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}, {1, 1}, {-1, -1}, {-1, 1}, {1, -1}};
    for (int step = 0; step < 64; step++) {
        const int* ray = rays[step % 8];
        a->s.step_phase = b->s.step_phase = serial->s.step_phase = (uint16_t)step;
        crossflow_step(a, ray[0], ray[1], true);
        crossflow_step(b, ray[0], ray[1], true);
        crossflow_step(serial, ray[0], ray[1], false);
        TEST_ASSERT_EQUAL_UINT(mass, crossflow_mass(a));
        TEST_ASSERT_EQUAL_UINT(mass, crossflow_mass(serial));
        TEST_ASSERT_EQUAL_MEMORY(a->cells, b->cells, sizeof a->cells);
        TEST_ASSERT_EQUAL_MEMORY(a->blocks, b->blocks, sizeof a->blocks);
    }
    free(a);
    free(b);
    free(serial);
}

static void
test_crossflow_uniform_pool_has_no_stripe_seams(void) {
    crossflow_fixture_t* f = crossflow_fixture();
    for (int y = 0; y < CF_H; y++) {
        sand_set(&f->s, 9, y, CELL_MAKE(MAT_WATER, 1 + y % 15));
    }
    const unsigned mass = crossflow_mass(f);
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
    const unsigned after = crossflow_mass(f);
    free(f);
    TEST_ASSERT_EQUAL_UINT(mass, after);
    TEST_ASSERT_LESS_OR_EQUAL_UINT_MESSAGE(1, max_jump,
                                           "neighboring rows, including seams, must level to within one mass unit");
}

void
run_sand_crossflow_suite(void) {
    RUN_TEST(test_split_crossflow_uses_hashed_viscosity);
    RUN_TEST(test_crossflow_seam_transfer_matches_serial_in_eight_directions);
    RUN_TEST(test_crossflow_pool_conserves_mass_and_is_deterministic);
    RUN_TEST(test_crossflow_uniform_pool_has_no_stripe_seams);
}

SUITE_REGISTER(run_sand_crossflow_suite);

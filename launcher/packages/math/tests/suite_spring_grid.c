#include <stdlib.h>
#include <string.h>

#include "math/motion/spring_grid.h"
#include "suites.h"
#include "unity.h"

#define SIDE    9
#define NODES   (SIDE * SIDE)
#define CENTRE  (SIDE / 2)
#define TENSION 64
#define TICKS   5000

static spring_grid_t
fixture(void) {
    spring_grid_t grid;
    int32_t* offset = malloc(NODES * sizeof(*offset));
    int32_t* velocity = malloc(NODES * sizeof(*velocity));
    TEST_ASSERT_NOT_NULL(offset);
    TEST_ASSERT_NOT_NULL(velocity);
    spring_grid_init(&grid, offset, velocity, SIDE, SIDE, TENSION, 0, 0);
    return grid;
}

static void
release(spring_grid_t* grid) {
    free(grid->offset);
    free(grid->velocity);
}

static int64_t
energy(const spring_grid_t* grid) {
    int64_t sum = 0;
    for (int y = 0; y < SIDE; y++) {
        for (int x = 0; x < SIDE; x++) {
            const int i = (y * SIDE) + x;
            const int64_t v = grid->velocity[i];
            const int64_t u = grid->offset[i];
            const int64_t dx = x + 1 < SIDE ? grid->offset[i + 1] - u : 0;
            const int64_t dy = y + 1 < SIDE ? grid->offset[i + SIDE] - u : 0;
            sum += (v * v) + (((dx * dx) + (dy * dy)) * grid->tension / SPRING_COEFFICIENT_ONE);
        }
    }
    return sum;
}

static void
test_poke_spreads_symmetrically(void) {
    spring_grid_t grid = fixture();
    spring_grid_poke(&grid, CENTRE, CENTRE, 2, MATHX_ONE);
    TEST_ASSERT_EQUAL_INT32(MATHX_ONE, grid.velocity[(CENTRE * SIDE) + CENTRE]);
    TEST_ASSERT_EQUAL_INT32(0, grid.velocity[(CENTRE * SIDE) + CENTRE + 2]);
    for (int tick = 0; tick < 4; tick++) {
        spring_grid_tick(&grid);
        for (int y = 0; y < SIDE; y++) {
            for (int x = 0; x < SIDE; x++) {
                const int32_t u = grid.offset[(y * SIDE) + x];
                TEST_ASSERT_EQUAL_INT32(u, grid.offset[(x * SIDE) + y]);
                TEST_ASSERT_EQUAL_INT32(u, grid.offset[(y * SIDE) + SIDE - 1 - x]);
            }
        }
    }
    TEST_ASSERT_NOT_EQUAL(0, grid.offset[(CENTRE * SIDE) + CENTRE + 2]);
    release(&grid);
}

static void
test_energy_is_bounded_and_damping_dissipates_it(void) {
    for (int damping = 0; damping <= 4; damping += 4) {
        spring_grid_t grid = fixture();
        grid.damping = damping;
        spring_grid_poke(&grid, CENTRE, CENTRE, 2, MATHX_ONE);
        /* Remove the rigid translation mode so the free sheet has bounded displacement. */
        int64_t total = 0;
        for (int i = 0; i < NODES; i++) {
            total += grid.velocity[i];
        }
        for (int i = 0; i < NODES; i++) {
            grid.velocity[i] -= (int32_t)(total / NODES);
        }
        const int64_t initial = energy(&grid);
        for (int tick = 0; tick < TICKS; tick++) {
            spring_grid_tick(&grid);
            TEST_ASSERT_TRUE(energy(&grid) < initial * 2);
            for (int i = 0; i < NODES; i++) {
                TEST_ASSERT_INT32_WITHIN(4 * MATHX_ONE, 0, grid.offset[i]);
            }
        }
        if (damping) {
            TEST_ASSERT_TRUE(energy(&grid) < initial / 100);
        }
        release(&grid);
    }
}

static void
test_ticks_are_deterministic(void) {
    spring_grid_t a = fixture();
    spring_grid_t b = fixture();
    for (int tick = 0; tick < 100; tick++) {
        spring_grid_poke(&a, tick % SIDE, (tick / SIDE) % SIDE, 3, MATHX_ONE / 8);
        spring_grid_poke(&b, tick % SIDE, (tick / SIDE) % SIDE, 3, MATHX_ONE / 8);
        spring_grid_tick(&a);
        spring_grid_tick(&b);
    }
    TEST_ASSERT_EQUAL_MEMORY(a.offset, b.offset, NODES * sizeof(*a.offset));
    TEST_ASSERT_EQUAL_MEMORY(a.velocity, b.velocity, NODES * sizeof(*a.velocity));
    release(&a);
    release(&b);
}

void
suite_spring_grid(void) {
    RUN_TEST(test_poke_spreads_symmetrically);
    RUN_TEST(test_energy_is_bounded_and_damping_dissipates_it);
    RUN_TEST(test_ticks_are_deterministic);
}

SUITE_REGISTER(suite_spring_grid);

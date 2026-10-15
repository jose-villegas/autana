#include <stdlib.h>

#include "boot/boot_anim.h"
#include "core/memory.h"
#include "suites.h"
#include "unity.h"

static void
test_replay_is_identical_at_every_frame_partition(void) {
    boot_anim_floor_t whole, split;
    boot_anim_floor_open(&whole);
    boot_anim_floor_open(&split);
    TEST_ASSERT_NOT_NULL(whole.grid.offset);
    TEST_ASSERT_NOT_NULL(split.grid.offset);
    boot_anim_floor_advance(&whole, BOOT_ANIM_MS);
    for (uint32_t ms = 0; ms < BOOT_ANIM_MS; ms += 7) {
        boot_anim_floor_advance(&split, ms);
    }
    boot_anim_floor_advance(&split, BOOT_ANIM_MS);
    TEST_ASSERT_EQUAL_UINT32(whole.ticks, split.ticks);
    TEST_ASSERT_EQUAL_INT(BOOT_ANIM_ZEROS, whole.next_zero);
    TEST_ASSERT_EQUAL_INT(whole.next_zero, split.next_zero);
    TEST_ASSERT_EQUAL_MEMORY(whole.grid.offset, split.grid.offset, BOOT_ANIM_FLOOR_NODES * sizeof(int32_t));
    TEST_ASSERT_EQUAL_MEMORY(whole.grid.velocity, split.grid.velocity, BOOT_ANIM_FLOOR_NODES * sizeof(int32_t));
    boot_anim_floor_close(&whole);
    boot_anim_floor_close(&split);
}

/* Every zero the pen passes moves the floor's place in the zero table on; only
 * one passed inside the pluck window kicks the grid. */
static void
test_each_zero_in_the_window_plucks_once_at_its_first_tick(void) {
    boot_anim_floor_t floor, reference;
    boot_anim_floor_open(&floor);
    boot_anim_floor_open(&reference);
    TEST_ASSERT_NOT_NULL(floor.grid.offset);
    TEST_ASSERT_NOT_NULL(reference.grid.offset);
    int next = 0;
    int plucked = 0;
    for (uint32_t ms = BOOT_ANIM_FLOOR_TICK_MS; ms <= BOOT_ANIM_MS; ms += BOOT_ANIM_FLOOR_TICK_MS) {
        const int32_t reached = boot_anim_pen_t(ms);
        const bool in_window = ms >= BOOT_ANIM_FLOOR_PLUCK_FROM_MS && ms < BOOT_ANIM_FLOOR_PLUCK_UNTIL_MS;
        while (next < BOOT_ANIM_ZEROS && boot_anim_zero_t[next] <= reached) {
            if (in_window) {
                spring_grid_poke(&reference.grid, BOOT_ANIM_MESH_CELLS / 2, BOOT_ANIM_MESH_CELLS / 2,
                                 BOOT_ANIM_FLOOR_PLUCK_CELLS, BOOT_ANIM_FLOOR_PLUCK_Q16);
                plucked++;
            }
            next++;
        }
        spring_grid_tick(&reference.grid);
        boot_anim_floor_advance(&floor, ms);
        TEST_ASSERT_EQUAL_INT(next, floor.next_zero);
        TEST_ASSERT_EQUAL_MEMORY(reference.grid.offset, floor.grid.offset, BOOT_ANIM_FLOOR_NODES * sizeof(int32_t));
        boot_anim_floor_advance(&floor, ms);
        TEST_ASSERT_EQUAL_INT(next, floor.next_zero);
        if (!plucked) {
            for (int i = 0; i < BOOT_ANIM_FLOOR_NODES; i++) {
                TEST_ASSERT_EQUAL_INT32(0, floor.grid.offset[i]);
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(BOOT_ANIM_ZEROS, next);
    boot_anim_floor_close(&floor);
    boot_anim_floor_close(&reference);
}

static void
test_allocation_failure_leaves_flat_floor(void) {
    void* held = memory_alloc(memory_largest_block(MEMORY_PSRAM), MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(held);
    boot_anim_floor_t floor;
    boot_anim_floor_open(&floor);
    memory_free(held);
    TEST_ASSERT_NULL(floor.grid.offset);
    TEST_ASSERT_NULL(floor.grid.velocity);
    boot_anim_floor_advance(&floor, BOOT_ANIM_MS);
    for (int j = 0; j <= BOOT_ANIM_MESH_CELLS; j++) {
        for (int i = 0; i <= BOOT_ANIM_MESH_CELLS; i++) {
            TEST_ASSERT_EQUAL_INT32(0, boot_anim_floor_height(&floor, i, j));
        }
    }
    boot_anim_floor_close(&floor);
}

void
suite_boot_anim_floor(void) {
    RUN_TEST(test_replay_is_identical_at_every_frame_partition);
    RUN_TEST(test_each_zero_in_the_window_plucks_once_at_its_first_tick);
    RUN_TEST(test_allocation_failure_leaves_flat_floor);
}

SUITE_REGISTER(suite_boot_anim_floor);

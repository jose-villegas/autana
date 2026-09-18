/* Portable checks for the seam-stall overlay data (sand.h):
 * sand_seam_guard_row_count()/sand_seam_guard_row()/sand_seam_stalled()/
 * sand_seam_stall_count(). suite_sand_two_core.c's own seam tests already
 * prove the guard pass never double-moves a grain; these check the bits it
 * exposes about that same decision match the columns it actually skipped. */
#include <stdlib.h>

#include "sand.h"
#include "sand_priv.h"
#include "suite_sand_common.h"
#include "suites.h"
#include "unity.h"

#define SO_W ((int)REAL_W)
#define SO_H ((int)REAL_H)

_Static_assert(SO_H >= SAND_BLOCK_H * 4, "the seam-overlay suite needs a grid tall enough to engage the checkerboard");

/* One throwaway zero-gravity call, the same trick suite_sand_two_core.c's
 * tc_prime_offset() uses: sand_step() increments step_phase before its
 * free-fall early return, so the step after it is the one these tests read. */
static void
prime_step_phase(sand_t* s) {
    sand_step(s, 0, 0, 0);
}

/* The first boundary of the NEXT step with room for a grain above its guard
 * row. Boundaries move with the hashed stripe offset, so a test cannot name
 * a row: it asks where they will be. */
static int
so_first_boundary(const sand_t* s) {
    /* sand_step() advances step_phase before it picks the offset, so the
     * boundary this reads is the one the NEXT call will use. */
    sand_t next = *s;
    next.step_phase++;
    const int offset = sand_stripe_offset(&next, SAND_BLOCK_H);
    int boundary = (offset > 0) ? offset : SAND_BLOCK_H;
    while (boundary < 4) {
        boundary += SAND_BLOCK_H;
    }
    return boundary;
}

static void
so_setup(sand_t* s, uint8_t* cells) {
    sand_init(s, cells, SO_W, SO_H, 1u);
    sand_set_scatter(s, 0);
    prime_step_phase(s);
}

static void
test_a_grain_crossing_into_a_guard_row_is_reported_as_stalled(void) {
    uint8_t* cells = malloc((size_t)SO_W * (size_t)SO_H);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t s;
    so_setup(&s, cells);

    /* A boundary's guard pair is (boundary - 1, boundary) - see
     * sweep_guard_row_list() in sand.c. One interior row above the guard row
     * falls straight into it this step, exactly the case
     * run_sweep_guard_rows()'s own comment describes. */
    const int x0 = SO_W / 2;
    const int boundary = so_first_boundary(&s);
    const int guard_row = boundary - 1;
    sand_set(&s, x0, guard_row - 1, SAND);

    const two_core_scope_t scope = two_core_scope_begin(true);
    sand_step(&s, 0, 1000, 0);
    two_core_scope_end(scope);

    TEST_ASSERT_TRUE_MESSAGE(sand_seam_guard_row_count() > 0, "two-core stepping on a tall grid must use guard rows");

    int guard_slot = -1;
    for (int i = 0; i < sand_seam_guard_row_count(); i++) {
        if (sand_seam_guard_row(i) == guard_row) {
            guard_slot = i;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(guard_slot >= 0, "the boundary's own guard row was not in this step's guard list");

    unsigned counted = 0;
    for (int i = 0; i < sand_seam_guard_row_count(); i++) {
        for (int x = 0; x < SO_W; x++) {
            const bool stalled = sand_seam_stalled(i, x);
            if (stalled) {
                counted++;
            }
            if (i == guard_slot && x == x0) {
                TEST_ASSERT_TRUE_MESSAGE(stalled, "the column the grain landed in via a phase-time move must stall");
            } else {
                TEST_ASSERT_FALSE_MESSAGE(stalled, "nothing else moved this step, so no other column may stall");
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, sand_seam_stall_count(), "exactly one column stalled this step");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(counted, sand_seam_stall_count(),
                                   "the per-column bits must sum to the reported count");

    free(cells);
}

static void
test_a_step_with_no_boundary_crossing_reports_no_stalls(void) {
    uint8_t* cells = malloc((size_t)SO_W * (size_t)SO_H);
    TEST_ASSERT_NOT_NULL(cells);

    sand_t s;
    so_setup(&s, cells);

    /* Well inside a stripe's interior at both ends of the fall - neither row
     * is a guard row, so this step's guard pass has nothing to skip. */
    const int start_row = so_first_boundary(&s) + 4;
    sand_set(&s, SO_W / 2, start_row, SAND);

    const two_core_scope_t scope = two_core_scope_begin(true);
    sand_step(&s, 0, 1000, 0);
    two_core_scope_end(scope);

    TEST_ASSERT_TRUE_MESSAGE(sand_seam_guard_row_count() > 0,
                             "two-core stepping on a tall grid must still use guard rows");

    for (int i = 0; i < sand_seam_guard_row_count(); i++) {
        for (int x = 0; x < SO_W; x++) {
            TEST_ASSERT_FALSE_MESSAGE(sand_seam_stalled(i, x), "no column crossed into a guard row this step");
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, sand_seam_stall_count(), "no boundary crossing must report zero stalls");

    free(cells);
}

void
run_sand_seam_overlay_suite(void) {
    RUN_TEST(test_a_grain_crossing_into_a_guard_row_is_reported_as_stalled);
    RUN_TEST(test_a_step_with_no_boundary_crossing_reports_no_stalls);
}

SUITE_REGISTER(run_sand_seam_overlay_suite);

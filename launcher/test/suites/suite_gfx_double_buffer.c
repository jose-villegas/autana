/* Portable suite: gfx_double_buffer's front/back ownership state machine. */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_double_buffer.h"

static void
test_double_buffer_targets_the_other_buffer_for_the_first_draw(void) {
    gfx_double_buffer_t state;

    gfx_double_buffer_begin(&state);

    TEST_ASSERT_EQUAL_UINT(0, gfx_double_buffer_present_index(&state));
    TEST_ASSERT_EQUAL_UINT(1, gfx_double_buffer_draw_index(&state));
    TEST_ASSERT_FALSE(gfx_double_buffer_primed(&state));
}

static void
test_double_buffer_swap_makes_the_completed_draw_the_next_present(void) {
    gfx_double_buffer_t state;

    gfx_double_buffer_begin(&state);
    gfx_double_buffer_swap(&state);

    TEST_ASSERT_EQUAL_UINT(1, gfx_double_buffer_present_index(&state));
    TEST_ASSERT_EQUAL_UINT(0, gfx_double_buffer_draw_index(&state));
    TEST_ASSERT_TRUE(gfx_double_buffer_primed(&state));
}

static void
test_double_buffer_alternates_the_present_and_draw_targets(void) {
    gfx_double_buffer_t state;

    gfx_double_buffer_begin(&state);
    gfx_double_buffer_swap(&state);
    gfx_double_buffer_swap(&state);

    TEST_ASSERT_EQUAL_UINT(0, gfx_double_buffer_present_index(&state));
    TEST_ASSERT_EQUAL_UINT(1, gfx_double_buffer_draw_index(&state));
}

void
run_gfx_double_buffer_suite(void) {
    RUN_TEST(test_double_buffer_targets_the_other_buffer_for_the_first_draw);
    RUN_TEST(test_double_buffer_swap_makes_the_completed_draw_the_next_present);
    RUN_TEST(test_double_buffer_alternates_the_present_and_draw_targets);
}

SUITE_REGISTER(run_gfx_double_buffer_suite);

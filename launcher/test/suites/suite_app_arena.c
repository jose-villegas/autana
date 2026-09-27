#include "suites.h"
#include "unity.h"

#include <stdint.h>

#include "app_arena.h"

static void
fixture(void) {
    app_arena_reset();
}

static void
test_every_power_of_two_alignment_is_honoured(void) {
    fixture();
    for (size_t align = 1; align <= 4096; align *= 2) {
        TEST_ASSERT_NOT_NULL(app_arena_take(1, 1)); /* knocks the offset off any boundary */
        const uintptr_t p = (uintptr_t)app_arena_take(3, align);
        TEST_ASSERT_NOT_EQUAL(0, p);
        TEST_ASSERT_EQUAL_UINT(0, p % align);
    }
}

static void
test_consecutive_blocks_do_not_overlap(void) {
    fixture();
    unsigned char* a = app_arena_take(100, 1);
    unsigned char* b = app_arena_take(100, 1);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(b >= a + 100);
    TEST_ASSERT_EQUAL_UINT(200, app_arena_used());
}

static void
test_the_whole_block_can_be_taken_at_once(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES, 1));
    TEST_ASSERT_NULL(app_arena_take(1, 1));
}

static void
test_exhaustion_refuses_without_taking_and_the_arena_stays_usable(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES - 64, 1));
    const size_t used = app_arena_used();

    TEST_ASSERT_NULL(app_arena_take(65, 1));
    TEST_ASSERT_NULL(app_arena_take(SIZE_MAX, 1));
    TEST_ASSERT_EQUAL_UINT(used, app_arena_used());

    TEST_ASSERT_NOT_NULL(app_arena_take(64, 1));
}

static void
test_alignment_padding_counts_against_what_is_left(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES - 8, 1));
    TEST_ASSERT_NULL(app_arena_take(8, 4096));
}

static void
test_zero_size_takes_nothing(void) {
    fixture();
    TEST_ASSERT_NULL(app_arena_take(0, 1));
    TEST_ASSERT_EQUAL_UINT(0, app_arena_used());
}

static void
test_an_alignment_that_is_not_a_power_of_two_is_refused(void) {
    fixture();
    TEST_ASSERT_NULL(app_arena_take(8, 0));
    TEST_ASSERT_NULL(app_arena_take(8, 3));
    TEST_ASSERT_NULL(app_arena_take(8, 24));
    TEST_ASSERT_EQUAL_UINT(0, app_arena_used());
}

static void
test_release_gives_back_everything_since_the_mark(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(10, 1));
    const app_arena_mark_t mark = app_arena_mark();
    void* first = app_arena_take(1000, 16);
    TEST_ASSERT_NOT_NULL(app_arena_take(5000, 16));

    app_arena_release(mark);
    TEST_ASSERT_EQUAL_UINT(10, app_arena_used());
    TEST_ASSERT_EQUAL_PTR(first, app_arena_take(1000, 16));
}

static void
test_nested_marks_release_inner_then_outer(void) {
    fixture();
    const app_arena_mark_t outer = app_arena_mark();
    TEST_ASSERT_NOT_NULL(app_arena_take(100, 1));
    const app_arena_mark_t inner = app_arena_mark();
    TEST_ASSERT_NOT_NULL(app_arena_take(100, 1));

    app_arena_release(inner);
    TEST_ASSERT_EQUAL_UINT(100, app_arena_used());
    app_arena_release(outer);
    TEST_ASSERT_EQUAL_UINT(0, app_arena_used());
}

static void
test_a_mark_from_before_a_reset_does_nothing(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(10, 1));
    const app_arena_mark_t stale = app_arena_mark();

    app_arena_reset();
    TEST_ASSERT_NOT_NULL(app_arena_take(40, 1));
    app_arena_release(stale);
    TEST_ASSERT_EQUAL_UINT(40, app_arena_used());
}

static void
test_reset_empties_the_arena_and_hands_out_the_same_memory_again(void) {
    fixture();
    void* first = app_arena_take(64, 64);
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES / 2, 1));

    app_arena_reset();
    TEST_ASSERT_EQUAL_UINT(0, app_arena_used());
    TEST_ASSERT_EQUAL_PTR(first, app_arena_take(64, 64));
}

void
run_app_arena_suite(void) {
    RUN_TEST(test_every_power_of_two_alignment_is_honoured);
    RUN_TEST(test_consecutive_blocks_do_not_overlap);
    RUN_TEST(test_the_whole_block_can_be_taken_at_once);
    RUN_TEST(test_exhaustion_refuses_without_taking_and_the_arena_stays_usable);
    RUN_TEST(test_alignment_padding_counts_against_what_is_left);
    RUN_TEST(test_zero_size_takes_nothing);
    RUN_TEST(test_an_alignment_that_is_not_a_power_of_two_is_refused);
    RUN_TEST(test_release_gives_back_everything_since_the_mark);
    RUN_TEST(test_nested_marks_release_inner_then_outer);
    RUN_TEST(test_a_mark_from_before_a_reset_does_nothing);
    RUN_TEST(test_reset_empties_the_arena_and_hands_out_the_same_memory_again);
}

SUITE_REGISTER(run_app_arena_suite);

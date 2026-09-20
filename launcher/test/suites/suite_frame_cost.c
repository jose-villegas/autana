/*
 * Portable suite: frame_cost - microseconds charged to named slots, read out
 * as milliseconds per frame.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "util/frame_cost.h"

static void
test_charges_to_one_name_add_up_and_the_report_is_per_frame(void) {
    frame_cost_t cost = {0};
    char line[128];
    frame_cost_add(&cost, "draw", 3000);
    frame_cost_add(&cost, "draw", 5000);
    frame_cost_add(&cost, "send", 16500);

    frame_cost_report(&cost, 2, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("draw 4.00/5.0  send 8.25/16.5 | total 12.25", line);
}

static void
test_a_name_is_one_slot_whether_or_not_it_is_the_same_literal(void) {
    frame_cost_t cost = {0};
    char line[64];
    char spelled_again[] = "draw";
    frame_cost_add(&cost, "draw", 1000);
    frame_cost_add(&cost, spelled_again, 1000);
    TEST_ASSERT_EQUAL_INT(1, cost.count);

    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("draw 2.00/1.0 | total 2.00", line);
}

static void
test_a_report_forgets_so_the_next_window_starts_clean(void) {
    frame_cost_t cost = {0};
    char line[64];
    frame_cost_add(&cost, "draw", 9000);
    frame_cost_report(&cost, 1, line, sizeof line);

    frame_cost_add(&cost, "draw", 1000);
    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("draw 1.00/1.0 | total 1.00", line);
}

static void
test_with_no_frames_the_charge_survives_to_the_next_report(void) {
    frame_cost_t cost = {0};
    char line[64] = "stale";
    frame_cost_add(&cost, "draw", 9000);
    TEST_ASSERT_EQUAL_INT(0, frame_cost_report(&cost, 0, line, sizeof line));
    TEST_ASSERT_EQUAL_STRING("", line);

    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("draw 9.00/9.0 | total 9.00", line);
}

static void
test_a_zero_size_buffer_is_left_alone(void) {
    frame_cost_t cost = {0};
    frame_cost_add(&cost, "draw", 9000);
    TEST_ASSERT_EQUAL_INT(0, frame_cost_report(&cost, 1, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, cost.count);
}

static void
test_more_names_than_slots_are_dropped_and_charged_to_nobody(void) {
    static const char* const names[FRAME_COST_SLOTS + 2] = {"a", "b", "c", "d", "e", "f", "g",
                                                            "h", "i", "j", "k", "l", "m", "n"};
    frame_cost_t cost = {0};
    for (int i = 0; i < FRAME_COST_SLOTS + 2; i++) {
        frame_cost_add(&cost, names[i], 1000);
    }
    TEST_ASSERT_EQUAL_INT(FRAME_COST_SLOTS, cost.count);
    TEST_ASSERT_EQUAL_INT(2, cost.dropped);
    for (int i = 0; i < FRAME_COST_SLOTS; i++) {
        TEST_ASSERT_EQUAL_INT64(1000, cost.slots[i].total_us);
    }
}

static void
test_a_dropped_name_is_flagged_in_the_report_and_gone_next_window(void) {
    static const char* const names[FRAME_COST_SLOTS] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"};
    frame_cost_t cost = {0};
    char line[300];
    for (int i = 0; i < FRAME_COST_SLOTS; i++) {
        frame_cost_add(&cost, names[i], 1000);
    }
    frame_cost_add(&cost, "m", 1000);

    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("a 1.00/1.0  b 1.00/1.0  c 1.00/1.0  d 1.00/1.0  e 1.00/1.0  f 1.00/1.0  g 1.00/1.0  "
                             "h 1.00/1.0  i 1.00/1.0  j 1.00/1.0  k 1.00/1.0  l 1.00/1.0 | total 12.00 +1 dropped",
                             line);

    frame_cost_add(&cost, "a", 1000);
    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("a 1.00/1.0 | total 1.00", line);
}

static void
test_a_line_too_short_ends_on_a_whole_slot(void) {
    frame_cost_t cost = {0};
    char line[20];
    frame_cost_add(&cost, "draw", 4000);
    frame_cost_add(&cost, "send", 8000);
    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("draw 4.00/4.0", line);
}

static void
test_the_total_is_the_sum_of_each_names_average(void) {
    frame_cost_t cost = {0};
    char line[128];
    frame_cost_add(&cost, "x", 1000);
    frame_cost_add(&cost, "y", 2500);
    frame_cost_add(&cost, "z", 4000);
    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("x 1.00/1.0  y 2.50/2.5  z 4.00/4.0 | total 7.50", line);
}

static void
test_a_bracket_inside_another_is_charged_only_its_own_time(void) {
    frame_cost_t cost = {0};
    char line[128];
    const int outer = frame_cost_enter(&cost, 0);
    const int inner = frame_cost_enter(&cost, 3000);
    frame_cost_leave(&cost, inner, "inner", 7000);
    frame_cost_leave(&cost, outer, "outer", 10000);

    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("inner 4.00/4.0  outer 6.00/6.0 | total 10.00", line);
}

static void
test_a_bracket_two_levels_deep_charges_each_level_its_own_time(void) {
    frame_cost_t cost = {0};
    char line[128];
    const int outer = frame_cost_enter(&cost, 0);
    const int middle = frame_cost_enter(&cost, 1000);
    const int inner = frame_cost_enter(&cost, 2000);
    frame_cost_leave(&cost, inner, "inner", 5000);
    frame_cost_leave(&cost, middle, "middle", 8000);
    frame_cost_leave(&cost, outer, "outer", 10000);

    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("inner 3.00/3.0  middle 4.00/4.0  outer 3.00/3.0 | total 10.00", line);
}

static void
test_two_siblings_inside_one_parent_are_both_taken_out_of_it(void) {
    frame_cost_t cost = {0};
    char line[128];
    const int outer = frame_cost_enter(&cost, 0);
    const int child_a = frame_cost_enter(&cost, 1000);
    frame_cost_leave(&cost, child_a, "childA", 3000);
    const int child_b = frame_cost_enter(&cost, 4000);
    frame_cost_leave(&cost, child_b, "childB", 9000);
    frame_cost_leave(&cost, outer, "outer", 10000);

    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("childA 2.00/2.0  childB 5.00/5.0  outer 3.00/3.0 | total 10.00", line);
}

static void
test_an_inner_bracket_whose_end_never_ran_is_discarded_when_the_outer_ends(void) {
    frame_cost_t cost = {0};
    char line[64];
    const int outer = frame_cost_enter(&cost, 0);
    frame_cost_enter(&cost, 2000); /* the inner bracket: its END never runs */
    frame_cost_leave(&cost, outer, "outer", 10000);

    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("outer 10.00/10.0 | total 10.00", line);

    const int next = frame_cost_enter(&cost, 11000);
    TEST_ASSERT_EQUAL_INT(0, next);
}

static void
test_what_ran_inside_an_abandoned_bracket_is_still_taken_out_of_the_outer_one(void) {
    frame_cost_t cost = {0};
    char line[128];
    const int outer = frame_cost_enter(&cost, 0);
    frame_cost_enter(&cost, 1000); /* abandoned: its END never runs */
    const int inner = frame_cost_enter(&cost, 2000);
    frame_cost_leave(&cost, inner, "inner", 6000);
    frame_cost_leave(&cost, outer, "outer", 10000);

    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("inner 4.00/4.0  outer 6.00/6.0 | total 10.00", line);
}

static void
test_leaving_with_a_mark_already_unwound_charges_nothing(void) {
    frame_cost_t cost = {0};
    char line[128];
    const int outer = frame_cost_enter(&cost, 0);
    const int inner = frame_cost_enter(&cost, 1000);
    frame_cost_leave(&cost, inner, "inner", 3000);
    frame_cost_leave(&cost, inner, "inner", 9000); /* stale: already popped */
    frame_cost_leave(&cost, outer, "outer", 10000);

    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("inner 2.00/2.0  outer 8.00/8.0 | total 10.00", line);
}

static void
test_a_ninth_nested_level_neither_crashes_nor_charges(void) {
    frame_cost_t cost = {0};
    char line[300];
    int marks[FRAME_COST_STACK_DEPTH];
    for (int i = 0; i < FRAME_COST_STACK_DEPTH; i++) {
        marks[i] = frame_cost_enter(&cost, 0);
    }
    const int ninth = frame_cost_enter(&cost, 0);
    TEST_ASSERT_EQUAL_INT(FRAME_COST_IGNORE_MARK, ninth);
    frame_cost_leave(&cost, ninth, "L8", 99999); /* charges nobody */

    static const char* const names[FRAME_COST_STACK_DEPTH] = {"L7", "L6", "L5", "L4", "L3", "L2", "L1", "L0"};
    for (int i = 0; i < FRAME_COST_STACK_DEPTH; i++) {
        frame_cost_leave(&cost, marks[FRAME_COST_STACK_DEPTH - 1 - i], names[i], (int64_t)(i + 1) * 1000);
    }

    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("L7 1.00/1.0  L6 1.00/1.0  L5 1.00/1.0  L4 1.00/1.0  L3 1.00/1.0  L2 1.00/1.0  "
                             "L1 1.00/1.0  L0 1.00/1.0 | total 8.00",
                             line);
}

static void
test_a_report_taken_with_brackets_still_open_empties_the_stack(void) {
    frame_cost_t cost = {0};
    char line[64];
    const int outer = frame_cost_enter(&cost, 0);
    frame_cost_add(&cost, "draw", 1000);
    frame_cost_report(&cost, 1, line, sizeof line);

    frame_cost_leave(&cost, outer, "outer", 5000);
    TEST_ASSERT_EQUAL_INT(0, cost.count);
}

void
suite_frame_cost(void) {
    RUN_TEST(test_charges_to_one_name_add_up_and_the_report_is_per_frame);
    RUN_TEST(test_a_name_is_one_slot_whether_or_not_it_is_the_same_literal);
    RUN_TEST(test_a_report_forgets_so_the_next_window_starts_clean);
    RUN_TEST(test_with_no_frames_the_charge_survives_to_the_next_report);
    RUN_TEST(test_a_zero_size_buffer_is_left_alone);
    RUN_TEST(test_more_names_than_slots_are_dropped_and_charged_to_nobody);
    RUN_TEST(test_a_dropped_name_is_flagged_in_the_report_and_gone_next_window);
    RUN_TEST(test_a_line_too_short_ends_on_a_whole_slot);
    RUN_TEST(test_the_total_is_the_sum_of_each_names_average);
    RUN_TEST(test_a_bracket_inside_another_is_charged_only_its_own_time);
    RUN_TEST(test_a_bracket_two_levels_deep_charges_each_level_its_own_time);
    RUN_TEST(test_two_siblings_inside_one_parent_are_both_taken_out_of_it);
    RUN_TEST(test_an_inner_bracket_whose_end_never_ran_is_discarded_when_the_outer_ends);
    RUN_TEST(test_what_ran_inside_an_abandoned_bracket_is_still_taken_out_of_the_outer_one);
    RUN_TEST(test_leaving_with_a_mark_already_unwound_charges_nothing);
    RUN_TEST(test_a_ninth_nested_level_neither_crashes_nor_charges);
    RUN_TEST(test_a_report_taken_with_brackets_still_open_empties_the_stack);
}

SUITE_REGISTER(suite_frame_cost);

/*
 * Portable suite: frame_cost, microseconds charged to named slots, read out
 * as milliseconds per frame.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "core/frame_cost.h"

/* A frame_cost_t is too big for the device's test stack: each test owns a
 * zeroed one on the heap and frees it before it returns. */
static frame_cost_t*
fixture(void) {
    frame_cost_t* const cost = calloc(1, sizeof *cost);
    TEST_ASSERT_NOT_NULL(cost);
    return cost;
}

static void
assert_report(frame_cost_t* cost, uint32_t frames, char* line, size_t capacity, const char* expected) {
    frame_cost_report(cost, frames, line, capacity);
    TEST_ASSERT_EQUAL_STRING(expected, line);
}

static int
leave_bracket(frame_cost_t* cost, const char* name, int64_t from_us, int64_t to_us) {
    const int mark = frame_cost_enter(cost, from_us);
    frame_cost_leave(cost, mark, name, to_us);
    return mark;
}

static void
test_charges_to_one_name_add_up_and_the_report_is_per_frame(void) {
    frame_cost_t* const cost = fixture();
    char line[128];
    frame_cost_add(cost, "draw", 3000);
    frame_cost_add(cost, "draw", 5000);
    frame_cost_add(cost, "send", 16500);

    assert_report(cost, 2, line, sizeof line, "draw 4.00/5.0  send 8.25/16.5 | total 12.25");
    free(cost);
}

static void
test_a_name_is_one_slot_whether_or_not_it_is_the_same_literal(void) {
    frame_cost_t* const cost = fixture();
    char line[64];
    char spelled_again[] = "draw";
    frame_cost_add(cost, "draw", 1000);
    frame_cost_add(cost, spelled_again, 1000);
    TEST_ASSERT_EQUAL_INT(1, cost->count);

    assert_report(cost, 1, line, sizeof line, "draw 2.00/1.0 | total 2.00");
    free(cost);
}

static void
test_a_report_forgets_so_the_next_window_starts_clean(void) {
    frame_cost_t* const cost = fixture();
    char line[64];
    frame_cost_add(cost, "draw", 9000);
    frame_cost_report(cost, 1, line, sizeof line);

    frame_cost_add(cost, "draw", 1000);
    assert_report(cost, 1, line, sizeof line, "draw 1.00/1.0 | total 1.00");
    free(cost);
}

static void
test_with_no_frames_the_charge_survives_to_the_next_report(void) {
    frame_cost_t* const cost = fixture();
    char line[64] = "stale";
    frame_cost_add(cost, "draw", 9000);
    TEST_ASSERT_EQUAL_INT(0, frame_cost_report(cost, 0, line, sizeof line));
    TEST_ASSERT_EQUAL_STRING("", line);

    assert_report(cost, 1, line, sizeof line, "draw 9.00/9.0 | total 9.00");
    free(cost);
}

static void
test_a_zero_size_buffer_is_left_alone(void) {
    frame_cost_t* const cost = fixture();
    frame_cost_add(cost, "draw", 9000);
    TEST_ASSERT_EQUAL_INT(0, frame_cost_report(cost, 1, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, cost->count);
    free(cost);
}

static void
test_more_names_than_slots_are_dropped_and_charged_to_nobody(void) {
    static const char* const names[FRAME_COST_SLOTS + 2] = {"a", "b", "c", "d", "e", "f", "g",
                                                            "h", "i", "j", "k", "l", "m", "n"};
    frame_cost_t* const cost = fixture();
    for (int i = 0; i < FRAME_COST_SLOTS + 2; i++) {
        frame_cost_add(cost, names[i], 1000);
    }
    TEST_ASSERT_EQUAL_INT(FRAME_COST_SLOTS, cost->count);
    TEST_ASSERT_EQUAL_INT(2, cost->dropped);
    for (int i = 0; i < FRAME_COST_SLOTS; i++) {
        TEST_ASSERT_EQUAL_INT64(1000, cost->slots[i].total_us);
    }
    free(cost);
}

static void
test_a_dropped_name_is_flagged_in_the_report_and_gone_next_window(void) {
    static const char* const names[FRAME_COST_SLOTS] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"};
    frame_cost_t* const cost = fixture();
    char line[300];
    for (int i = 0; i < FRAME_COST_SLOTS; i++) {
        frame_cost_add(cost, names[i], 1000);
    }
    frame_cost_add(cost, "m", 1000);

    assert_report(cost, 1, line, sizeof line,
                  "a 1.00/1.0  b 1.00/1.0  c 1.00/1.0  d 1.00/1.0  e 1.00/1.0  f 1.00/1.0  g 1.00/1.0  "
                  "h 1.00/1.0  i 1.00/1.0  j 1.00/1.0  k 1.00/1.0  l 1.00/1.0 | total 12.00 +1 dropped");

    frame_cost_add(cost, "a", 1000);
    assert_report(cost, 1, line, sizeof line, "a 1.00/1.0 | total 1.00");
    free(cost);
}

static void
test_a_line_too_short_ends_on_a_whole_slot(void) {
    frame_cost_t* const cost = fixture();
    char line[20];
    frame_cost_add(cost, "draw", 4000);
    frame_cost_add(cost, "send", 8000);
    assert_report(cost, 1, line, sizeof line, "draw 4.00/4.0");
    free(cost);
}

static void
test_the_total_is_the_sum_of_each_names_average(void) {
    frame_cost_t* const cost = fixture();
    char line[128];
    frame_cost_add(cost, "x", 1000);
    frame_cost_add(cost, "y", 2500);
    frame_cost_add(cost, "z", 4000);
    assert_report(cost, 1, line, sizeof line, "x 1.00/1.0  y 2.50/2.5  z 4.00/4.0 | total 7.50");
    free(cost);
}

static void
test_a_bracket_inside_another_is_charged_only_its_own_time(void) {
    frame_cost_t* const cost = fixture();
    char line[128];
    const int outer = frame_cost_enter(cost, 0);
    (void)leave_bracket(cost, "inner", 3000, 7000);
    frame_cost_leave(cost, outer, "outer", 10000);

    assert_report(cost, 1, line, sizeof line, "inner 4.00/4.0  outer 6.00/6.0 | total 10.00");
    free(cost);
}

static void
test_a_bracket_two_levels_deep_charges_each_level_its_own_time(void) {
    frame_cost_t* const cost = fixture();
    char line[128];
    const int outer = frame_cost_enter(cost, 0);
    const int middle = frame_cost_enter(cost, 1000);
    (void)leave_bracket(cost, "inner", 2000, 5000);
    frame_cost_leave(cost, middle, "middle", 8000);
    frame_cost_leave(cost, outer, "outer", 10000);

    assert_report(cost, 1, line, sizeof line, "inner 3.00/3.0  middle 4.00/4.0  outer 3.00/3.0 | total 10.00");
    free(cost);
}

static void
test_two_siblings_inside_one_parent_are_both_taken_out_of_it(void) {
    frame_cost_t* const cost = fixture();
    char line[128];
    const int outer = frame_cost_enter(cost, 0);
    (void)leave_bracket(cost, "childA", 1000, 3000);
    (void)leave_bracket(cost, "childB", 4000, 9000);
    frame_cost_leave(cost, outer, "outer", 10000);

    assert_report(cost, 1, line, sizeof line, "childA 2.00/2.0  childB 5.00/5.0  outer 3.00/3.0 | total 10.00");
    free(cost);
}

static void
test_an_inner_bracket_whose_end_never_ran_is_discarded_when_the_outer_ends(void) {
    frame_cost_t* const cost = fixture();
    char line[64];
    const int outer = frame_cost_enter(cost, 0);
    frame_cost_enter(cost, 2000); /* the inner bracket: its END never runs */
    frame_cost_leave(cost, outer, "outer", 10000);

    assert_report(cost, 1, line, sizeof line, "outer 10.00/10.0 | total 10.00");

    const int next = frame_cost_enter(cost, 11000);
    TEST_ASSERT_EQUAL_INT(0, next);
    free(cost);
}

static void
test_what_ran_inside_an_abandoned_bracket_is_still_taken_out_of_the_outer_one(void) {
    frame_cost_t* const cost = fixture();
    char line[128];
    const int outer = frame_cost_enter(cost, 0);
    frame_cost_enter(cost, 1000); /* abandoned: its END never runs */
    (void)leave_bracket(cost, "inner", 2000, 6000);
    frame_cost_leave(cost, outer, "outer", 10000);

    assert_report(cost, 1, line, sizeof line, "inner 4.00/4.0  outer 6.00/6.0 | total 10.00");
    free(cost);
}

static void
test_leaving_with_a_mark_already_unwound_charges_nothing(void) {
    frame_cost_t* const cost = fixture();
    char line[128];
    const int outer = frame_cost_enter(cost, 0);
    const int inner = leave_bracket(cost, "inner", 1000, 3000);
    frame_cost_leave(cost, inner, "inner", 9000); /* stale: already popped */
    frame_cost_leave(cost, outer, "outer", 10000);

    assert_report(cost, 1, line, sizeof line, "inner 2.00/2.0  outer 8.00/8.0 | total 10.00");
    free(cost);
}

static void
test_a_ninth_nested_level_neither_crashes_nor_charges(void) {
    frame_cost_t* const cost = fixture();
    char line[300];
    int marks[FRAME_COST_STACK_DEPTH];
    for (int i = 0; i < FRAME_COST_STACK_DEPTH; i++) {
        marks[i] = frame_cost_enter(cost, 0);
    }
    const int ninth = frame_cost_enter(cost, 0);
    TEST_ASSERT_EQUAL_INT(FRAME_COST_IGNORE_MARK, ninth);
    frame_cost_leave(cost, ninth, "L8", 99999); /* charges nobody */

    static const char* const names[FRAME_COST_STACK_DEPTH] = {"L7", "L6", "L5", "L4", "L3", "L2", "L1", "L0"};
    for (int i = 0; i < FRAME_COST_STACK_DEPTH; i++) {
        frame_cost_leave(cost, marks[FRAME_COST_STACK_DEPTH - 1 - i], names[i], (int64_t)(i + 1) * 1000);
    }

    assert_report(cost, 1, line, sizeof line,
                  "L7 1.00/1.0  L6 1.00/1.0  L5 1.00/1.0  L4 1.00/1.0  L3 1.00/1.0  L2 1.00/1.0  "
                  "L1 1.00/1.0  L0 1.00/1.0 | total 8.00");
    free(cost);
}

static void
test_a_report_taken_with_brackets_still_open_empties_the_stack(void) {
    frame_cost_t* const cost = fixture();
    char line[64];
    const int outer = frame_cost_enter(cost, 0);
    frame_cost_add(cost, "draw", 1000);
    frame_cost_report(cost, 1, line, sizeof line);

    frame_cost_leave(cost, outer, "outer", 5000);
    TEST_ASSERT_EQUAL_INT(0, cost->count);
    free(cost);
}

static void
leave_counted(frame_cost_t* cost, const char* name, uint32_t cycles_from, uint32_t cycles_to, uint32_t event_from,
              uint32_t event_to) {
    const int mark = frame_cost_enter_counted(cost, 0, cycles_from, event_from);
    frame_cost_leave_counted(cost, mark, name, 1000, cycles_to, event_to);
}

static void
leave_nested_counted(frame_cost_t* cost) {
    const int outer = frame_cost_enter_counted(cost, 0, 0, 0);
    const int inner = frame_cost_enter_counted(cost, 100, 400, 40);
    frame_cost_leave_counted(cost, inner, "stage.inner", 300, 900, 90);
    frame_cost_leave_counted(cost, outer, "stage.outer", 500, 1000, 100);
}

/* Arms `name`, noting it first as a bracket's first END would, the way the
 * console's request and the frame task's apply do. */
static void
arm(frame_cost_t* cost, const char* name, int event_index) {
    frame_cost_note_name(cost, name);
    frame_cost_post_arm(cost, frame_cost_name_index(cost, name), event_index);
    int applied = -2;
    TEST_ASSERT_TRUE(frame_cost_apply_pending(cost, true, &applied));
    TEST_ASSERT_EQUAL_INT(event_index, applied);
}

static void
test_an_armed_name_accumulates_its_count_sum_extremes_and_event(void) {
    frame_cost_t* const cost = fixture();
    arm(cost, "stage.a", 0);

    leave_counted(cost, "stage.a", 0, 300, 0, 100);
    leave_counted(cost, "stage.a", 1000, 1100, 500, 560);
    leave_counted(cost, "stage.a", 2000, 2700, 900, 1040);

    const frame_cost_slot_t* slot = &cost->slots[0];
    TEST_ASSERT_EQUAL_UINT32(3, slot->n);
    TEST_ASSERT_EQUAL_UINT64(1100, slot->cycles_sum);
    TEST_ASSERT_EQUAL_UINT32(100, slot->cycles_min);
    TEST_ASSERT_EQUAL_UINT32(700, slot->cycles_max);
    TEST_ASSERT_EQUAL_UINT64(300, slot->event_sum);
    free(cost);
}

static void
test_counters_that_wrap_between_a_begin_and_an_end_still_difference_right(void) {
    frame_cost_t* const cost = fixture();
    arm(cost, "stage.a", 0);

    leave_counted(cost, "stage.a", 0xFFFFFF00u, 0x00000100u, 0xFFFFFFF0u, 0x00000010u);

    TEST_ASSERT_EQUAL_UINT64(0x200, cost->slots[0].cycles_sum);
    TEST_ASSERT_EQUAL_UINT64(0x20, cost->slots[0].event_sum);
    free(cost);
}

static void
test_only_the_armed_name_takes_counter_samples(void) {
    frame_cost_t* const cost = fixture();
    arm(cost, "stage.a", 0);

    leave_counted(cost, "stage.b", 0, 500, 0, 50);
    leave_counted(cost, "stage.a", 0, 200, 0, 20);

    TEST_ASSERT_EQUAL_UINT32(0, cost->slots[0].n);
    TEST_ASSERT_EQUAL_UINT32(1, cost->slots[1].n);
    free(cost);
}

static void
test_a_bracket_begun_before_the_arm_takes_no_sample(void) {
    frame_cost_t* const cost = fixture();
    frame_cost_note_name(cost, "stage.a");
    const int mark = frame_cost_enter_counted(cost, 0, 0, 0);
    frame_cost_post_arm(cost, 0, 0);
    int event = 0;
    TEST_ASSERT_FALSE(frame_cost_apply_pending(cost, true, &event));
    frame_cost_leave_counted(cost, mark, "stage.a", 1000, 900, 90);

    TEST_ASSERT_EQUAL_UINT32(0, cost->slots[0].n);
    free(cost);
}

static void
test_counts_nest_like_time_the_outer_bracket_keeps_only_its_own(void) {
    frame_cost_t* const cost = fixture();
    arm(cost, "stage.outer", 0);

    leave_nested_counted(cost);

    const frame_cost_slot_t* outer_slot = &cost->slots[1];
    TEST_ASSERT_EQUAL_STRING("stage.outer", outer_slot->name);
    TEST_ASSERT_EQUAL_UINT64(500, outer_slot->cycles_sum);
    TEST_ASSERT_EQUAL_UINT64(50, outer_slot->event_sum);
    free(cost);
}

static void
test_what_ran_inside_an_abandoned_bracket_is_taken_out_of_the_armed_outer_counts(void) {
    frame_cost_t* const cost = fixture();
    arm(cost, "stage.outer", 0);

    const int outer = frame_cost_enter_counted(cost, 0, 0, 0);
    (void)frame_cost_enter_counted(cost, 100, 100, 10);
    const int inner = frame_cost_enter_counted(cost, 150, 200, 20);
    frame_cost_leave_counted(cost, inner, "stage.inner", 250, 500, 60);
    frame_cost_leave_counted(cost, outer, "stage.outer", 500, 1000, 100);

    const frame_cost_slot_t* outer_slot = &cost->slots[1];
    TEST_ASSERT_EQUAL_UINT64(700, outer_slot->cycles_sum);
    TEST_ASSERT_EQUAL_UINT64(60, outer_slot->event_sum);
    free(cost);
}

static void
test_an_armed_inner_bracket_samples_its_own_counts_and_the_outer_loses_them(void) {
    frame_cost_t* const cost = fixture();
    arm(cost, "stage.inner", 0);

    leave_nested_counted(cost);

    TEST_ASSERT_EQUAL_STRING("stage.inner", cost->slots[0].name);
    TEST_ASSERT_EQUAL_UINT64(500, cost->slots[0].cycles_sum);
    TEST_ASSERT_EQUAL_UINT32(0, cost->slots[1].n);
    free(cost);
}

static void
test_an_armed_name_nested_in_itself_samples_each_level_exclusively(void) {
    frame_cost_t* const cost = fixture();
    arm(cost, "stage.a", 0);

    const int outer = frame_cost_enter_counted(cost, 0, 0, 0);
    const int inner = frame_cost_enter_counted(cost, 100, 200, 20);
    frame_cost_leave_counted(cost, inner, "stage.a", 200, 500, 50);
    frame_cost_leave_counted(cost, outer, "stage.a", 500, 1000, 100);

    TEST_ASSERT_EQUAL_UINT32(2, cost->slots[0].n);
    TEST_ASSERT_EQUAL_UINT64(1000, cost->slots[0].cycles_sum);
    TEST_ASSERT_EQUAL_UINT32(300, cost->slots[0].cycles_min);
    TEST_ASSERT_EQUAL_UINT32(700, cost->slots[0].cycles_max);
    free(cost);
}

static void
test_a_request_is_applied_between_outermost_brackets_only(void) {
    frame_cost_t* const cost = fixture();
    frame_cost_note_name(cost, "stage.a");
    int event = -2;

    const int outer = frame_cost_enter_counted(cost, 0, 0, 0);
    frame_cost_post_arm(cost, 0, 3);
    TEST_ASSERT_FALSE(frame_cost_apply_pending(cost, true, &event));
    TEST_ASSERT_EQUAL_INT(0, cost->armed_name);
    frame_cost_leave_counted(cost, outer, "stage.x", 10, 0, 0);

    TEST_ASSERT_TRUE(frame_cost_apply_pending(cost, true, &event));
    TEST_ASSERT_EQUAL_INT(3, event);
    TEST_ASSERT_EQUAL_INT(1, cost->armed_name);
    TEST_ASSERT_FALSE_MESSAGE(frame_cost_apply_pending(cost, true, &event), "a request is applied once");
    free(cost);
}

static void
test_leaving_a_bracket_yields_its_whole_elapsed_time_nested_ones_included(void) {
    frame_cost_t* const cost = fixture();
    const int outer = frame_cost_enter(cost, 1000);
    const int inner = frame_cost_enter(cost, 1200);

    TEST_ASSERT_EQUAL_INT64(300, frame_cost_leave_counted(cost, inner, "stage.inner", 1500, 0, 0));
    TEST_ASSERT_EQUAL_INT64(900, frame_cost_leave_counted(cost, outer, "stage.outer", 1900, 0, 0));
    free(cost);
}

static void
test_leaving_with_a_mark_that_is_gone_yields_nothing(void) {
    frame_cost_t* const cost = fixture();

    TEST_ASSERT_EQUAL_INT64(0, frame_cost_leave_counted(cost, FRAME_COST_IGNORE_MARK, "stage.a", 500, 0, 0));
    TEST_ASSERT_EQUAL_INT64(0, frame_cost_leave_counted(cost, 0, "stage.a", 500, 0, 0));
    free(cost);
}

static void
test_the_newest_request_wins(void) {
    frame_cost_t* const cost = fixture();
    frame_cost_note_name(cost, "stage.a");
    frame_cost_note_name(cost, "stage.b");
    int event = -2;

    frame_cost_post_arm(cost, 0, 1);
    frame_cost_post_arm(cost, 1, 2);
    TEST_ASSERT_TRUE(frame_cost_apply_pending(cost, true, &event));

    TEST_ASSERT_EQUAL_INT(2, cost->armed_name);
    TEST_ASSERT_EQUAL_INT(2, event);
    free(cost);
}

static void
test_an_empty_request_disarms_and_sampling_stops(void) {
    frame_cost_t* const cost = fixture();
    arm(cost, "stage.a", 0);
    leave_counted(cost, "stage.a", 0, 100, 0, 10);
    int event = 0;

    frame_cost_post_arm(cost, -1, 0);
    TEST_ASSERT_TRUE(frame_cost_apply_pending(cost, true, &event));
    TEST_ASSERT_EQUAL_INT(-1, event);
    leave_counted(cost, "stage.a", 0, 100, 0, 10);

    TEST_ASSERT_EQUAL_UINT32(0, cost->slots[0].n);
    free(cost);
}

static void
test_a_task_that_does_not_own_the_frame_neither_applies_nor_clears_a_request(void) {
    frame_cost_t* const cost = fixture();
    frame_cost_note_name(cost, "stage.a");
    frame_cost_post_arm(cost, 0, 4);
    int event = -2;

    TEST_ASSERT_FALSE(frame_cost_apply_pending(cost, false, &event));
    TEST_ASSERT_EQUAL_INT(0, cost->armed_name);
    TEST_ASSERT_TRUE(frame_cost_apply_pending(cost, true, &event));
    TEST_ASSERT_EQUAL_INT(4, event);
    free(cost);
}

static void
test_a_change_of_arm_drops_the_samples_it_would_have_mislabelled(void) {
    frame_cost_t* const cost = fixture();
    char line[FRAME_COST_COUNTS_MAX];
    arm(cost, "stage.a", 0);
    leave_counted(cost, "stage.a", 0, 100, 0, 10);
    TEST_ASSERT_GREATER_THAN_INT(0, frame_cost_counts_line(cost, "insn", line, sizeof line));

    arm(cost, "stage.a", 1);

    TEST_ASSERT_EQUAL_UINT32(0, cost->slots[0].n);
    TEST_ASSERT_EQUAL_INT(0, frame_cost_counts_line(cost, "window", line, sizeof line));
    free(cost);
}

static void
test_a_disarm_drops_the_samples_too(void) {
    frame_cost_t* const cost = fixture();
    char line[FRAME_COST_COUNTS_MAX];
    arm(cost, "stage.a", 0);
    leave_counted(cost, "stage.a", 0, 100, 0, 10);
    int event = 0;

    frame_cost_post_arm(cost, -1, 0);
    TEST_ASSERT_TRUE(frame_cost_apply_pending(cost, true, &event));

    TEST_ASSERT_EQUAL_UINT32(0, cost->slots[0].n);
    TEST_ASSERT_EQUAL_INT(0, frame_cost_counts_line(cost, "insn", line, sizeof line));
    free(cost);
}

static void
test_an_armed_name_with_no_free_slot_is_dropped_once_not_twice(void) {
    static const char* const names[FRAME_COST_SLOTS] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"};
    frame_cost_t* const cost = fixture();
    for (int i = 0; i < FRAME_COST_SLOTS; i++) {
        frame_cost_add(cost, names[i], 1);
    }
    arm(cost, "m", 0);

    leave_counted(cost, "m", 0, 100, 0, 10);

    TEST_ASSERT_EQUAL_INT(1, cost->dropped);
    free(cost);
}

static void
test_a_name_is_looked_up_for_the_table_once_a_window_not_at_every_end(void) {
    char names[FRAME_COST_NAMES][8];
    frame_cost_t* const cost = fixture();
    for (int i = 0; i < FRAME_COST_NAMES; i++) {
        snprintf(names[i], sizeof names[i], "n%d", i);
        frame_cost_note_name(cost, names[i]);
    }

    for (int i = 0; i < 50; i++) {
        frame_cost_add(cost, "late", 10);
    }

    TEST_ASSERT_EQUAL_INT(1, cost->names_dropped);
    free(cost);
}

static void
test_the_counts_line_carries_the_armed_slot_and_the_time_line_does_not(void) {
    frame_cost_t* const cost = fixture();
    char counts[FRAME_COST_COUNTS_MAX];
    char line[FRAME_COST_REPORT_MAX];
    arm(cost, "stage.a", 0);
    leave_counted(cost, "stage.a", 0, 300, 0, 100);
    leave_counted(cost, "stage.a", 0, 100, 0, 60);

    TEST_ASSERT_GREATER_THAN_INT(0, frame_cost_counts_line(cost, "insn", counts, sizeof counts));
    TEST_ASSERT_EQUAL_STRING("perf: stage.a cyc avg/min/max 200/100/300 insn avg 80 n=2", counts);
    frame_cost_report(cost, 1, line, sizeof line);
    TEST_ASSERT_NULL(strstr(line, "cyc"));

    leave_counted(cost, "stage.b", 0, 100, 0, 10);
    TEST_ASSERT_EQUAL_INT(0, frame_cost_counts_line(cost, "insn", counts, sizeof counts));
    free(cost);
}

static void
test_a_longest_name_and_largest_counts_fit_their_buffers(void) {
    char name[FRAME_COST_NAME_MAX + 1];
    memset(name, 'n', FRAME_COST_NAME_MAX);
    name[FRAME_COST_NAME_MAX] = '\0';
    frame_cost_t* const cost = fixture();
    char counts[FRAME_COST_COUNTS_MAX];
    arm(cost, name, 0);
    leave_counted(cost, name, 0, 0xfffffff0u, 0, 0xfffffff0u);

    TEST_ASSERT_GREATER_THAN_INT(0, frame_cost_counts_line(cost, "icache_miss_stall", counts, sizeof counts));
    TEST_ASSERT_NOT_NULL(strstr(counts, " n=1"));
    free(cost);
}

static void
test_every_slot_with_the_longest_name_still_reports_its_total_and_drops(void) {
    frame_cost_t* const cost = fixture();
    char* const line = malloc(FRAME_COST_REPORT_MAX);
    TEST_ASSERT_NOT_NULL(line);
    char names[FRAME_COST_SLOTS + 1][FRAME_COST_NAME_MAX + 1];
    for (int i = 0; i <= FRAME_COST_SLOTS; i++) {
        memset(names[i], 'a' + i, FRAME_COST_NAME_MAX);
        names[i][FRAME_COST_NAME_MAX] = '\0';
        frame_cost_add(cost, names[i], 9999999999LL);
    }

    TEST_ASSERT_GREATER_THAN_INT(0, frame_cost_report(cost, 1, line, FRAME_COST_REPORT_MAX));
    TEST_ASSERT_NOT_NULL(strstr(line, " | total "));
    TEST_ASSERT_NOT_NULL(strstr(line, " +1 dropped"));
    free(line);
    free(cost);
}

static void
test_a_name_is_remembered_across_windows_and_found_by_its_text(void) {
    frame_cost_t* const cost = fixture();
    char line[64];
    char spelled_again[] = "stage.a";
    frame_cost_add(cost, "stage.a", 100);
    frame_cost_report(cost, 1, line, sizeof line);

    TEST_ASSERT_EQUAL_INT(0, frame_cost_name_index(cost, spelled_again));
    TEST_ASSERT_EQUAL_INT(-1, frame_cost_name_index(cost, "stage."));
    TEST_ASSERT_EQUAL_INT(1, cost->name_count);
    free(cost);
}

static void
test_more_names_than_the_memory_holds_are_counted_not_lost_silently(void) {
    frame_cost_t* const cost = fixture();
    char names[FRAME_COST_NAMES + 2][8];
    for (int i = 0; i < FRAME_COST_NAMES + 2; i++) {
        snprintf(names[i], sizeof names[i], "n%d", i);
        frame_cost_note_name(cost, names[i]);
    }
    TEST_ASSERT_EQUAL_INT(FRAME_COST_NAMES, cost->name_count);
    TEST_ASSERT_EQUAL_INT(2, cost->names_dropped);
    TEST_ASSERT_EQUAL_INT(-1, frame_cost_name_index(cost, names[FRAME_COST_NAMES]));
    free(cost);
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
    RUN_TEST(test_an_armed_name_accumulates_its_count_sum_extremes_and_event);
    RUN_TEST(test_counters_that_wrap_between_a_begin_and_an_end_still_difference_right);
    RUN_TEST(test_only_the_armed_name_takes_counter_samples);
    RUN_TEST(test_a_bracket_begun_before_the_arm_takes_no_sample);
    RUN_TEST(test_counts_nest_like_time_the_outer_bracket_keeps_only_its_own);
    RUN_TEST(test_what_ran_inside_an_abandoned_bracket_is_taken_out_of_the_armed_outer_counts);
    RUN_TEST(test_an_armed_inner_bracket_samples_its_own_counts_and_the_outer_loses_them);
    RUN_TEST(test_an_armed_name_nested_in_itself_samples_each_level_exclusively);
    RUN_TEST(test_a_request_is_applied_between_outermost_brackets_only);
    RUN_TEST(test_leaving_a_bracket_yields_its_whole_elapsed_time_nested_ones_included);
    RUN_TEST(test_leaving_with_a_mark_that_is_gone_yields_nothing);
    RUN_TEST(test_the_newest_request_wins);
    RUN_TEST(test_an_empty_request_disarms_and_sampling_stops);
    RUN_TEST(test_a_task_that_does_not_own_the_frame_neither_applies_nor_clears_a_request);
    RUN_TEST(test_a_change_of_arm_drops_the_samples_it_would_have_mislabelled);
    RUN_TEST(test_a_disarm_drops_the_samples_too);
    RUN_TEST(test_an_armed_name_with_no_free_slot_is_dropped_once_not_twice);
    RUN_TEST(test_a_name_is_looked_up_for_the_table_once_a_window_not_at_every_end);
    RUN_TEST(test_the_counts_line_carries_the_armed_slot_and_the_time_line_does_not);
    RUN_TEST(test_a_longest_name_and_largest_counts_fit_their_buffers);
    RUN_TEST(test_every_slot_with_the_longest_name_still_reports_its_total_and_drops);
    RUN_TEST(test_a_name_is_remembered_across_windows_and_found_by_its_text);
    RUN_TEST(test_more_names_than_the_memory_holds_are_counted_not_lost_silently);
}

SUITE_REGISTER(suite_frame_cost);

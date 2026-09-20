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
    TEST_ASSERT_EQUAL_STRING("draw 4.00/5.0  send 8.25/16.5", line);
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
    TEST_ASSERT_EQUAL_STRING("draw 2.00/1.0", line);
}

static void
test_a_report_forgets_so_the_next_window_starts_clean(void) {
    frame_cost_t cost = {0};
    char line[64];
    frame_cost_add(&cost, "draw", 9000);
    frame_cost_report(&cost, 1, line, sizeof line);

    frame_cost_add(&cost, "draw", 1000);
    frame_cost_report(&cost, 1, line, sizeof line);
    TEST_ASSERT_EQUAL_STRING("draw 1.00/1.0", line);
}

static void
test_with_no_frames_there_is_nothing_to_say(void) {
    frame_cost_t cost = {0};
    char line[64] = "stale";
    frame_cost_add(&cost, "draw", 9000);
    TEST_ASSERT_EQUAL_INT(0, frame_cost_report(&cost, 0, line, sizeof line));
    TEST_ASSERT_EQUAL_STRING("", line);
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
    for (int i = 0; i < FRAME_COST_SLOTS; i++) {
        TEST_ASSERT_EQUAL_INT64(1000, cost.slots[i].total_us);
    }
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

void
suite_frame_cost(void) {
    RUN_TEST(test_charges_to_one_name_add_up_and_the_report_is_per_frame);
    RUN_TEST(test_a_name_is_one_slot_whether_or_not_it_is_the_same_literal);
    RUN_TEST(test_a_report_forgets_so_the_next_window_starts_clean);
    RUN_TEST(test_with_no_frames_there_is_nothing_to_say);
    RUN_TEST(test_more_names_than_slots_are_dropped_and_charged_to_nobody);
    RUN_TEST(test_a_line_too_short_ends_on_a_whole_slot);
}

SUITE_REGISTER(suite_frame_cost);

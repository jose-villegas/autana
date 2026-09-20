/*
 * Portable suite: tune - the console protocol for changing a number on a
 * running device, driven here with lines and a reply callback.
 */

#include <stdio.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "util/tune.h"

#define REPLIES_MAX 8

static char replies[REPLIES_MAX][96];
static int reply_count;

static void
collect(const char* line) {
    if (reply_count < REPLIES_MAX) {
        strncpy(replies[reply_count], line, sizeof replies[0] - 1);
        replies[reply_count][sizeof replies[0] - 1] = '\0';
    }
    reply_count++;
}

static int32_t trail;
static int32_t radius;

static void
fixture(void) {
    tune_reset();
    reply_count = 0;
    trail = 226;
    radius = 13;
    TEST_ASSERT_TRUE(tune_register("launcher.ridge_trail", &trail, 0, 255));
    TEST_ASSERT_TRUE(tune_register("launcher.glow_radius", &radius, 1, 31));
}

static bool
say(const char* line) {
    reply_count = 0;
    return tune_handle_line(line, collect);
}

static void
test_set_changes_the_variable_and_says_so(void) {
    fixture();
    TEST_ASSERT_TRUE(say("SET launcher.ridge_trail 200"));
    TEST_ASSERT_EQUAL_INT32(200, trail);
    TEST_ASSERT_EQUAL_INT(1, reply_count);
    TEST_ASSERT_EQUAL_STRING("TUNE_OK launcher.ridge_trail=200", replies[0]);
    TEST_ASSERT_EQUAL_INT32(13, radius);
}

static void
test_get_reads_without_changing_anything(void) {
    fixture();
    const uint32_t before = tune_generation();
    TEST_ASSERT_TRUE(say("GET launcher.glow_radius"));
    TEST_ASSERT_EQUAL_STRING("TUNE_OK launcher.glow_radius=13", replies[0]);
    TEST_ASSERT_EQUAL_UINT32(before, tune_generation());
}

static void
test_the_generation_counts_sets_that_took_and_no_others(void) {
    fixture();
    const uint32_t before = tune_generation();
    say("SET launcher.ridge_trail 1");
    say("SET launcher.ridge_trail 999");
    say("SET nothing.here 1");
    say("GET launcher.ridge_trail");
    say("SET launcher.glow_radius 20");
    TEST_ASSERT_EQUAL_UINT32(before + 2, tune_generation());
}

static void
test_a_value_out_of_range_is_refused_and_names_the_range(void) {
    fixture();
    say("SET launcher.ridge_trail 256");
    TEST_ASSERT_EQUAL_INT32(226, trail);
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR range launcher.ridge_trail takes 0..255", replies[0]);
    say("SET launcher.glow_radius 0");
    TEST_ASSERT_EQUAL_INT32(13, radius);
    say("SET launcher.ridge_trail 255");
    TEST_ASSERT_EQUAL_INT32(255, trail);
    say("SET launcher.ridge_trail 0");
    TEST_ASSERT_EQUAL_INT32(0, trail);
}

static void
test_a_bad_line_changes_nothing_and_says_why(void) {
    fixture();
    say("SET launcher.ridge_trail");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR usage SET <name> <value>", replies[0]);
    say("SET launcher.ridge_trail twelve");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR not-a-number twelve", replies[0]);
    say("SET launcher.ridge_trail 12px");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR not-a-number 12px", replies[0]);
    say("SET launcher.ridge_trai 12");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR unknown launcher.ridge_trai", replies[0]);
    say("GET launcher.nope");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR unknown launcher.nope", replies[0]);
    TEST_ASSERT_EQUAL_INT32(226, trail);
}

static void
test_hex_is_taken_so_a_colour_can_be_typed_as_one(void) {
    fixture();
    static int32_t colour = 0xFFFFFF;
    tune_register("launcher.glow_halo", &colour, 0, 0xFFFFFF);
    say("SET launcher.glow_halo 0x38D6E8");
    TEST_ASSERT_EQUAL_HEX32(0x38D6E8, colour);
}

static void
test_tune_lists_every_tunable_and_then_the_count(void) {
    fixture();
    TEST_ASSERT_TRUE(say("TUNE"));
    TEST_ASSERT_EQUAL_INT(3, reply_count);
    TEST_ASSERT_EQUAL_STRING("TUNE launcher.ridge_trail=226 min=0 max=255", replies[0]);
    TEST_ASSERT_EQUAL_STRING("TUNE launcher.glow_radius=13 min=1 max=31", replies[1]);
    TEST_ASSERT_EQUAL_STRING("TUNE_END count=2", replies[2]);
}

static void
test_lines_that_are_not_for_it_are_left_alone(void) {
    fixture();
    TEST_ASSERT_FALSE(say("SCREENSHOT"));
    TEST_ASSERT_FALSE(say("BUILDID"));
    TEST_ASSERT_FALSE(say("RUNSUITE run_tune_suite"));
    TEST_ASSERT_FALSE(say("TUNES"));
    TEST_ASSERT_FALSE(say("SETTLE"));
    TEST_ASSERT_FALSE(say(""));
    TEST_ASSERT_EQUAL_INT(0, reply_count);
}

static void
test_registering_a_name_again_moves_it_and_a_long_name_is_refused(void) {
    fixture();
    static int32_t other = 7;
    TEST_ASSERT_TRUE(tune_register("launcher.ridge_trail", &other, 0, 9));
    TEST_ASSERT_EQUAL_INT(2, tune_count());
    say("SET launcher.ridge_trail 5");
    TEST_ASSERT_EQUAL_INT32(5, other);
    TEST_ASSERT_EQUAL_INT32(226, trail);
    TEST_ASSERT_FALSE(tune_register("a.name.far.too.long.to.fit.in.one.console.line", &other, 0, 1));
}

void
suite_tune(void) {
    RUN_TEST(test_set_changes_the_variable_and_says_so);
    RUN_TEST(test_get_reads_without_changing_anything);
    RUN_TEST(test_the_generation_counts_sets_that_took_and_no_others);
    RUN_TEST(test_a_value_out_of_range_is_refused_and_names_the_range);
    RUN_TEST(test_a_bad_line_changes_nothing_and_says_why);
    RUN_TEST(test_hex_is_taken_so_a_colour_can_be_typed_as_one);
    RUN_TEST(test_tune_lists_every_tunable_and_then_the_count);
    RUN_TEST(test_lines_that_are_not_for_it_are_left_alone);
    RUN_TEST(test_registering_a_name_again_moves_it_and_a_long_name_is_refused);
}

SUITE_REGISTER(suite_tune);

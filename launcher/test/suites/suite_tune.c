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
static tune_owner_t ridge_owner;
static tune_owner_t other_owner;
static tune_entry_t trail_entry;
static tune_entry_t radius_entry;
static tune_registry_t registry;

static void
fixture(void) {
    registry = (tune_registry_t){0};
    ridge_owner = (tune_owner_t){0};
    other_owner = (tune_owner_t){0};
    reply_count = 0;
    trail = 226;
    radius = 13;
    trail_entry = (tune_entry_t){"ridge.trail", &trail, 226, 0, 255, &ridge_owner, NULL};
    radius_entry = (tune_entry_t){"glow.radius", &radius, 13, 1, 31, &other_owner, NULL};
    TEST_ASSERT_TRUE(tune_register(&registry, &trail_entry));
    TEST_ASSERT_TRUE(tune_register(&registry, &radius_entry));
}

static bool
say(const char* line) {
    reply_count = 0;
    return tune_registry_handle_line(&registry, line, collect);
}

static void
test_set_changes_the_variable_and_says_so(void) {
    fixture();
    TEST_ASSERT_TRUE(say("SET ridge.trail 200"));
    TEST_ASSERT_EQUAL_INT32(200, trail);
    TEST_ASSERT_EQUAL_INT(1, reply_count);
    TEST_ASSERT_EQUAL_STRING("TUNE_OK ridge.trail=200", replies[0]);
    TEST_ASSERT_EQUAL_INT32(13, radius);
}

static void
test_get_reads_without_changing_anything(void) {
    fixture();
    TEST_ASSERT_TRUE(say("GET glow.radius"));
    TEST_ASSERT_EQUAL_STRING("TUNE_OK glow.radius=13", replies[0]);
    TEST_ASSERT_EQUAL_UINT32(0, other_owner.generation);
}

static void
test_a_generation_counts_its_own_owners_changes_and_no_others(void) {
    fixture();
    say("SET ridge.trail 1");
    say("SET ridge.trail 999");
    say("SET nothing.here 1");
    say("GET ridge.trail");
    say("RESET ridge.trail");
    TEST_ASSERT_EQUAL_UINT32(2, ridge_owner.generation);
    TEST_ASSERT_EQUAL_UINT32(0, other_owner.generation);
    say("SET glow.radius 20");
    TEST_ASSERT_EQUAL_UINT32(2, ridge_owner.generation);
    TEST_ASSERT_EQUAL_UINT32(1, other_owner.generation);
}

static void
test_reset_puts_back_the_value_it_was_declared_with(void) {
    fixture();
    say("SET ridge.trail 40");
    TEST_ASSERT_TRUE(say("RESET ridge.trail"));
    TEST_ASSERT_EQUAL_INT32(226, trail);
    TEST_ASSERT_EQUAL_STRING("TUNE_OK ridge.trail=226", replies[0]);
    say("RESET ridge.nope");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR unknown ridge.nope", replies[0]);
}

static void
test_a_value_out_of_range_is_refused_and_names_the_range(void) {
    fixture();
    say("SET ridge.trail 256");
    TEST_ASSERT_EQUAL_INT32(226, trail);
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR range ridge.trail takes 0..255", replies[0]);
    say("SET glow.radius 0");
    TEST_ASSERT_EQUAL_INT32(13, radius);
    say("SET ridge.trail 255");
    TEST_ASSERT_EQUAL_INT32(255, trail);
    say("SET ridge.trail 0");
    TEST_ASSERT_EQUAL_INT32(0, trail);
}

static void
test_a_bad_line_changes_nothing_and_says_why(void) {
    fixture();
    say("SET ridge.trail");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR usage SET <name> <value>", replies[0]);
    say("SET ridge.trail twelve");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR not-a-number twelve", replies[0]);
    say("SET ridge.trail 12px");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR not-a-number 12px", replies[0]);
    say("SET ridge.trai 12");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR unknown ridge.trai", replies[0]);
    say("GET ridge.nope");
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR unknown ridge.nope", replies[0]);
    TEST_ASSERT_EQUAL_INT32(226, trail);
}

static void
test_hex_is_taken_so_a_colour_can_be_typed_as_one(void) {
    fixture();
    static int32_t colour;
    static tune_entry_t colour_entry;
    colour = 0xFFFFFF;
    colour_entry = (tune_entry_t){"glow.halo", &colour, 0xFFFFFF, 0, 0xFFFFFF, NULL, NULL};
    tune_register(&registry, &colour_entry);
    say("SET glow.halo 0x38D6E8");
    TEST_ASSERT_EQUAL_HEX32(0x38D6E8, colour);
}

static void
test_tune_lists_every_tunable_by_name_with_its_default_and_then_the_count(void) {
    fixture();
    /* Registered last and listed in the middle: neither end of the list. */
    static int32_t shift;
    static tune_entry_t shift_entry;
    shift = -4;
    shift_entry = (tune_entry_t){"hue.shift", &shift, -4, -8, 8, NULL, NULL};
    tune_register(&registry, &shift_entry);

    say("SET ridge.trail 200");
    TEST_ASSERT_TRUE(say("TUNE"));
    TEST_ASSERT_EQUAL_INT(4, reply_count);
    TEST_ASSERT_EQUAL_STRING("TUNE glow.radius=13 min=1 max=31 default=13", replies[0]);
    TEST_ASSERT_EQUAL_STRING("TUNE hue.shift=-4 min=-8 max=8 default=-4", replies[1]);
    TEST_ASSERT_EQUAL_STRING("TUNE ridge.trail=200 min=0 max=255 default=226", replies[2]);
    TEST_ASSERT_EQUAL_STRING("TUNE_END count=3", replies[3]);
}

static void
test_lines_that_are_not_for_it_are_left_alone(void) {
    fixture();
    TEST_ASSERT_FALSE(say("SCREENSHOT"));
    TEST_ASSERT_FALSE(say("BUILDID"));
    TEST_ASSERT_FALSE(say("RUNSUITE run_tune_suite"));
    TEST_ASSERT_FALSE(say("TUNES"));
    TEST_ASSERT_FALSE(say("SETTLE"));
    TEST_ASSERT_FALSE(say("RESETTLE"));
    TEST_ASSERT_FALSE(say(""));
    TEST_ASSERT_EQUAL_INT(0, reply_count);
}

static void
test_a_name_declared_twice_stays_with_the_first_and_the_listing_says_so(void) {
    fixture();
    static int32_t other;
    static tune_entry_t usurper;
    other = 7;
    usurper = (tune_entry_t){"ridge.trail", &other, 7, 0, 9, NULL, NULL};
    TEST_ASSERT_FALSE(tune_register(&registry, &usurper));
    TEST_ASSERT_FALSE(tune_register(&registry, &usurper));
    TEST_ASSERT_EQUAL_INT(2, tune_count(&registry));
    say("SET ridge.trail 5");
    TEST_ASSERT_EQUAL_INT32(5, trail);
    TEST_ASSERT_EQUAL_INT32(7, other);

    say("TUNE");
    TEST_ASSERT_EQUAL_INT(4, reply_count);
    TEST_ASSERT_EQUAL_STRING("TUNE_ERR clash ridge.trail", replies[2]);
}

static void
test_registering_an_entry_again_changes_nothing(void) {
    fixture();
    TEST_ASSERT_TRUE(tune_register(&registry, &trail_entry));
    TEST_ASSERT_TRUE(tune_register(&registry, &radius_entry));
    TEST_ASSERT_EQUAL_INT(2, tune_count(&registry));
}

/* The declaration an owner writes, as one writes it. */
TUNE_OWNER(tune_suite);
TUNE(tune_suite, knob, 5, 0, 9);

static void
test_a_declared_tunable_is_in_the_shared_registry_before_anything_runs(void) {
    reply_count = 0;
    const tune_entry_t* entry = tune_find(tune_shared(), "tune_suite.knob");
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_INT32(5, entry->initial);

    const uint32_t before = TUNE_GENERATION(tune_suite);
    TEST_ASSERT_TRUE(tune_handle_line("SET tune_suite.knob 8", collect));
    TEST_ASSERT_EQUAL_INT32(8, knob);
    TEST_ASSERT_EQUAL_UINT32(before + 1, TUNE_GENERATION(tune_suite));
    tune_handle_line("RESET tune_suite.knob", collect);
    TEST_ASSERT_EQUAL_INT32(5, knob);
}

void
suite_tune(void) {
    RUN_TEST(test_set_changes_the_variable_and_says_so);
    RUN_TEST(test_get_reads_without_changing_anything);
    RUN_TEST(test_a_generation_counts_its_own_owners_changes_and_no_others);
    RUN_TEST(test_reset_puts_back_the_value_it_was_declared_with);
    RUN_TEST(test_a_value_out_of_range_is_refused_and_names_the_range);
    RUN_TEST(test_a_bad_line_changes_nothing_and_says_why);
    RUN_TEST(test_hex_is_taken_so_a_colour_can_be_typed_as_one);
    RUN_TEST(test_tune_lists_every_tunable_by_name_with_its_default_and_then_the_count);
    RUN_TEST(test_lines_that_are_not_for_it_are_left_alone);
    RUN_TEST(test_a_name_declared_twice_stays_with_the_first_and_the_listing_says_so);
    RUN_TEST(test_registering_an_entry_again_changes_nothing);
    RUN_TEST(test_a_declared_tunable_is_in_the_shared_registry_before_anything_runs);
}

SUITE_REGISTER(suite_tune);

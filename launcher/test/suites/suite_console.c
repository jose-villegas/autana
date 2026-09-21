/*
 * Portable suite: console_verbs (dispatch, registration discipline),
 * console_latch (the frame-loop handoff) and console_app_line (an
 * unclaimed line's route to the running app), driven here with a registry
 * and latches this suite owns - never console_shared(), which only a
 * device build's CONSOLE_VERB() entries ever touch. Also the TOUCH/IMU
 * parsers (console_inject_parse.h), moved here from suite_build_id.c.
 */

#include <stdio.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "app.h"
#include "console/console_app_line.h"
#include "console/console_inject_parse.h"
#include "console/console_latch.h"
#include "console/console_verbs.h"

#define REPLIES_MAX 8

static char replies[REPLIES_MAX][64];
static int reply_count;

static void
collect(const char* line) {
    if (reply_count < REPLIES_MAX) {
        strncpy(replies[reply_count], line, sizeof replies[0] - 1);
        replies[reply_count][sizeof replies[0] - 1] = '\0';
    }
    reply_count++;
}

static char last_verb[16];
static char last_args[32];

static void
record(const char* verb, const char* args) {
    strncpy(last_verb, verb, sizeof last_verb - 1);
    last_verb[sizeof last_verb - 1] = '\0';
    strncpy(last_args, args, sizeof last_args - 1);
    last_args[sizeof last_args - 1] = '\0';
}

static void
handle_alpha(const char* args, console_reply_fn reply) {
    (void)reply;
    record("ALPHA", args);
}

static void
handle_beta(const char* args, console_reply_fn reply) {
    (void)reply;
    record("BETA", args);
}

static void
handle_set(const char* args, console_reply_fn reply) {
    record("SET", args);
    reply("SET_OK");
}

static void
handle_settle(const char* args, console_reply_fn reply) {
    (void)reply;
    record("SETTLE", args);
}

static void
handle_tune(const char* args, console_reply_fn reply) {
    (void)reply;
    record("TUNE", args);
}

static void
handle_tunes(const char* args, console_reply_fn reply) {
    (void)reply;
    record("TUNES", args);
}

static console_registry_t registry;
static console_verb_t alpha_verb, beta_verb, set_verb, settle_verb, tune_verb, tunes_verb, lower_set_verb;

static void
fixture(void) {
    registry = (console_registry_t){0};
    reply_count = 0;
    last_verb[0] = '\0';
    last_args[0] = '\0';
    alpha_verb = (console_verb_t){"ALPHA", handle_alpha, NULL};
    beta_verb = (console_verb_t){"BETA", handle_beta, NULL};
    set_verb = (console_verb_t){"SET", handle_set, NULL};
    settle_verb = (console_verb_t){"SETTLE", handle_settle, NULL};
    tune_verb = (console_verb_t){"TUNE", handle_tune, NULL};
    tunes_verb = (console_verb_t){"TUNES", handle_tunes, NULL};
    lower_set_verb = (console_verb_t){"set", handle_alpha, NULL};
}

static bool
say(const char* line) {
    reply_count = 0;
    return console_registry_handle_line(&registry, line, collect);
}

static int
registry_count(const console_registry_t* reg) {
    int n = 0;
    for (const console_verb_t* entry = reg->first; entry != NULL; entry = entry->next) {
        n++;
    }
    return n;
}

static void
test_exact_name_matches(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    TEST_ASSERT_TRUE(say("ALPHA"));
    TEST_ASSERT_EQUAL_STRING("ALPHA", last_verb);
    TEST_ASSERT_EQUAL_STRING("", last_args);
}

static void
test_name_plus_space_carries_args_without_the_verb_or_the_space(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    TEST_ASSERT_TRUE(say("ALPHA one two"));
    TEST_ASSERT_EQUAL_STRING("ALPHA", last_verb);
    TEST_ASSERT_EQUAL_STRING("one two", last_args);
}

/* SET must not answer to SETTLE, and TUNE must not answer to TUNES - the
 * same pair suite_tune.c already pins one level up (util/tune.c). Proven
 * here too since this level is where the "name, or name-plus-space" rule
 * actually lives now. */
static void
test_a_verb_does_not_swallow_a_longer_word_that_starts_with_its_name(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &set_verb));
    TEST_ASSERT_TRUE(console_register(&registry, &settle_verb));
    TEST_ASSERT_TRUE(console_register(&registry, &tune_verb));
    TEST_ASSERT_TRUE(console_register(&registry, &tunes_verb));

    TEST_ASSERT_TRUE(say("SETTLE now"));
    TEST_ASSERT_EQUAL_STRING("SETTLE", last_verb);
    TEST_ASSERT_TRUE(say("TUNES"));
    TEST_ASSERT_EQUAL_STRING("TUNES", last_verb);
    TEST_ASSERT_TRUE(say("SET a 1"));
    TEST_ASSERT_EQUAL_STRING("SET", last_verb);
    TEST_ASSERT_TRUE(say("TUNE"));
    TEST_ASSERT_EQUAL_STRING("TUNE", last_verb);
}

/* The registered name and the typed line are matched folded, so the
 * lowercase name a person types and the uppercase one a harness has
 * always sent reach the same verb. */
static void
test_a_verb_answers_whatever_case_it_is_typed_in(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));

    TEST_ASSERT_TRUE(say("alpha one two"));
    TEST_ASSERT_EQUAL_STRING("ALPHA", last_verb);
    TEST_ASSERT_EQUAL_STRING("one two", last_args);

    TEST_ASSERT_TRUE(say("AlPhA"));
    TEST_ASSERT_EQUAL_STRING("ALPHA", last_verb);
    TEST_ASSERT_EQUAL_STRING("", last_args);
}

/* Two names that differ only in case are one name, so the second is the
 * clash console_register() already refuses - otherwise it would register
 * and then never be reached, since the first one matches every line. */
static void
test_a_name_already_taken_in_another_case_is_refused(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &set_verb));
    TEST_ASSERT_FALSE(console_register(&registry, &lower_set_verb));
    TEST_ASSERT_EQUAL_INT(1, registry_count(&registry));

    TEST_ASSERT_TRUE(say("set a 1"));
    TEST_ASSERT_EQUAL_STRING("SET", last_verb);
}

/* The longer-word rule holds across case too: folding must not turn
 * "settle" into a line SET answers. */
static void
test_the_longer_word_rule_holds_across_case(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &set_verb));
    TEST_ASSERT_TRUE(console_register(&registry, &settle_verb));

    TEST_ASSERT_TRUE(say("settle now"));
    TEST_ASSERT_EQUAL_STRING("SETTLE", last_verb);
    TEST_ASSERT_TRUE(say("Set a 1"));
    TEST_ASSERT_EQUAL_STRING("SET", last_verb);
}

static void
test_an_unknown_line_is_left_alone(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    TEST_ASSERT_FALSE(say("GAMMA"));
    TEST_ASSERT_FALSE(say("ALPHABET one"));
    TEST_ASSERT_FALSE(say(""));
    TEST_ASSERT_EQUAL_INT(0, reply_count);
}

/* Scrambled registration order on purpose: a fixture that happened to
 * register in sorted order once let a link-order bug pass by arrangement
 * (see suite_tune.c's own "declared last, listed in the middle" case). */
static void
test_registration_order_does_not_change_dispatch_or_listing_order(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &tune_verb));
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    TEST_ASSERT_TRUE(console_register(&registry, &settle_verb));
    TEST_ASSERT_TRUE(console_register(&registry, &beta_verb));

    TEST_ASSERT_EQUAL_STRING("ALPHA", registry.first->name);
    TEST_ASSERT_EQUAL_STRING("BETA", registry.first->next->name);
    TEST_ASSERT_EQUAL_STRING("SETTLE", registry.first->next->next->name);
    TEST_ASSERT_EQUAL_STRING("TUNE", registry.first->next->next->next->name);
    TEST_ASSERT_NULL(registry.first->next->next->next->next);

    TEST_ASSERT_TRUE(say("BETA"));
    TEST_ASSERT_EQUAL_STRING("BETA", last_verb);
}

static void
test_a_name_declared_twice_stays_with_the_first(void) {
    fixture();
    static console_verb_t usurper;
    usurper = (console_verb_t){"ALPHA", handle_beta, NULL};
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    TEST_ASSERT_FALSE(console_register(&registry, &usurper));
    TEST_ASSERT_EQUAL_INT(1, registry_count(&registry));
    TEST_ASSERT_TRUE(say("ALPHA"));
    TEST_ASSERT_EQUAL_STRING("ALPHA", last_verb);
}

static void
test_registering_the_same_verb_object_twice_is_once(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    TEST_ASSERT_EQUAL_INT(1, registry_count(&registry));
}

static void
test_a_reply_is_up_to_the_verbs_own_handler(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &set_verb));
    TEST_ASSERT_TRUE(say("SET x 1"));
    TEST_ASSERT_EQUAL_INT(1, reply_count);
    TEST_ASSERT_EQUAL_STRING("SET_OK", replies[0]);
}

static void
test_a_latch_is_taken_once_and_carries_its_args(void) {
    console_latch_t latch = {0};
    char out[CONSOLE_ARGS_MAX];

    TEST_ASSERT_FALSE(console_latch_take(&latch, out, sizeof out));

    console_latch_set(&latch, "run_gfx_suite");
    TEST_ASSERT_TRUE(latch.pending);
    TEST_ASSERT_TRUE(console_latch_take(&latch, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("run_gfx_suite", out);
    TEST_ASSERT_FALSE(latch.pending);
    TEST_ASSERT_FALSE(console_latch_take(&latch, out, sizeof out));
}

static void
test_a_second_set_before_a_take_keeps_the_newer_args(void) {
    console_latch_t latch = {0};
    char out[CONSOLE_ARGS_MAX];

    console_latch_set(&latch, "first");
    console_latch_set(&latch, "second");
    TEST_ASSERT_TRUE(console_latch_take(&latch, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("second", out);
}

static void
test_a_latch_truncates_args_that_do_not_fit_without_overflowing(void) {
    console_latch_t latch = {0};
    char out[CONSOLE_ARGS_MAX];
    char long_args[CONSOLE_ARGS_MAX + 32];

    memset(long_args, 'x', sizeof long_args - 1);
    long_args[sizeof long_args - 1] = '\0';

    console_latch_set(&latch, long_args);
    TEST_ASSERT_EQUAL_INT((int)sizeof(latch.args) - 1, (int)strlen(latch.args));
    TEST_ASSERT_TRUE(console_latch_take(&latch, out, sizeof out));
    TEST_ASSERT_EQUAL_INT((int)sizeof out - 1, (int)strlen(out));
}

static void
test_taking_into_a_smaller_buffer_truncates_too(void) {
    console_latch_t latch = {0};
    char small[4];

    console_latch_set(&latch, "abcdef");
    TEST_ASSERT_TRUE(console_latch_take(&latch, small, sizeof small));
    TEST_ASSERT_EQUAL_STRING("abc", small);
}

/* console_dispatch_or_latch() is what console.c's reader task actually
 * calls for every line - registry first, so a verb an app also happens to
 * spell the same way still wins and never even reaches the latch. */
static void
test_dispatch_or_latch_prefers_a_registered_verb_over_latching(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    console_latch_t unclaimed = {0};

    TEST_ASSERT_TRUE(console_dispatch_or_latch(&registry, "ALPHA", collect, &unclaimed));
    TEST_ASSERT_EQUAL_STRING("ALPHA", last_verb);
    TEST_ASSERT_FALSE_MESSAGE(unclaimed.pending, "a verb that claimed the line must never reach the latch");
}

static void
test_dispatch_or_latch_latches_a_line_no_verb_claims(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    console_latch_t unclaimed = {0};
    char out[CONSOLE_ARGS_MAX];

    TEST_ASSERT_FALSE(console_dispatch_or_latch(&registry, "counts", collect, &unclaimed));
    TEST_ASSERT_TRUE(console_latch_take(&unclaimed, out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("counts", out);
}

static char captured_app_line[64];
static bool app_line_claims;

static bool
fake_app_console_line(const char* line) {
    strncpy(captured_app_line, line, sizeof captured_app_line - 1);
    captured_app_line[sizeof captured_app_line - 1] = '\0';
    return app_line_claims;
}

/* console_app_line_offer() is the frame loop's own half - what main.c calls
 * once it has taken the latch above. */
static void
test_app_line_offer_reaches_a_running_apps_callback(void) {
    app_line_claims = true;
    captured_app_line[0] = '\0';
    const app_t app = {.console_line = fake_app_console_line};

    TEST_ASSERT_TRUE(console_app_line_offer(&app, "counts"));
    TEST_ASSERT_EQUAL_STRING("counts", captured_app_line);
}

static void
test_app_line_offer_is_false_without_an_app_a_callback_or_a_claim(void) {
    app_line_claims = true;
    TEST_ASSERT_FALSE_MESSAGE(console_app_line_offer(NULL, "counts"), "no app is running");

    const app_t no_callback = {0};
    TEST_ASSERT_FALSE_MESSAGE(console_app_line_offer(&no_callback, "counts"), "the app set no console_line");

    app_line_claims = false;
    const app_t declines = {.console_line = fake_app_console_line};
    TEST_ASSERT_FALSE_MESSAGE(console_app_line_offer(&declines, "counts"), "the callback itself declined it");
}

static void
test_touch_line_carries_its_sample(void) {
    bool down = false;
    int x = 0, y = 0;

    TEST_ASSERT_TRUE(console_touch_parse("down 184 224", &down, &x, &y));
    TEST_ASSERT_TRUE(down);
    TEST_ASSERT_EQUAL_INT(184, x);
    TEST_ASSERT_EQUAL_INT(224, y);

    TEST_ASSERT_TRUE(console_touch_parse("up 7 9", &down, &x, &y));
    TEST_ASSERT_FALSE(down);
    TEST_ASSERT_EQUAL_INT(7, x);
    TEST_ASSERT_EQUAL_INT(9, y);
}

/* A half-read sample is worse than none: the caller would move a finger
 * somewhere nobody asked for. */
static void
test_a_malformed_touch_line_changes_nothing(void) {
    bool down = true;
    int x = 11, y = 22;

    TEST_ASSERT_FALSE(console_touch_parse("left 1 2", &down, &x, &y));
    TEST_ASSERT_FALSE(console_touch_parse("down 1", &down, &x, &y));
    TEST_ASSERT_FALSE(console_touch_parse(" ", &down, &x, &y));
    TEST_ASSERT_TRUE(down);
    TEST_ASSERT_EQUAL_INT(11, x);
    TEST_ASSERT_EQUAL_INT(22, y);
}

/* The reader task assembles a line into a buffer this long, so a sample
 * that cannot fit it is a sample that never arrives whole. */
static void
test_a_touch_line_fits_the_console_line(void) {
    char line[CONSOLE_LINE_MAX];
    const int written = snprintf(line, sizeof line, "TOUCH down %d %d", 448, 448);
    TEST_ASSERT_TRUE(written > 0 && written < (int)sizeof line);
}

static void
test_an_imu_line_carries_its_sample(void) {
    int ax = 0, ay = 0, az = 0;
    TEST_ASSERT_TRUE(console_imu_parse("4096 -12 0", &ax, &ay, &az));
    TEST_ASSERT_EQUAL_INT(4096, ax);
    TEST_ASSERT_EQUAL_INT(-12, ay);
    TEST_ASSERT_EQUAL_INT(0, az);
}

static void
test_a_malformed_imu_line_changes_nothing(void) {
    int ax = 7, ay = 8, az = 9;
    TEST_ASSERT_FALSE(console_imu_parse("4096 0", &ax, &ay, &az));
    TEST_ASSERT_FALSE(console_imu_parse("1 2 3 4", &ax, &ay, &az));
    TEST_ASSERT_FALSE_MESSAGE(console_imu_parse("40000 0 0", &ax, &ay, &az),
                              "a count past int16 is not a sample the sensor could give");
    TEST_ASSERT_TRUE_MESSAGE(ax == 7 && ay == 8 && az == 9, "a rejected line must leave the outputs alone");
}

void
suite_console(void) {
    RUN_TEST(test_exact_name_matches);
    RUN_TEST(test_name_plus_space_carries_args_without_the_verb_or_the_space);
    RUN_TEST(test_a_verb_does_not_swallow_a_longer_word_that_starts_with_its_name);
    RUN_TEST(test_a_verb_answers_whatever_case_it_is_typed_in);
    RUN_TEST(test_a_name_already_taken_in_another_case_is_refused);
    RUN_TEST(test_the_longer_word_rule_holds_across_case);
    RUN_TEST(test_an_unknown_line_is_left_alone);
    RUN_TEST(test_registration_order_does_not_change_dispatch_or_listing_order);
    RUN_TEST(test_a_name_declared_twice_stays_with_the_first);
    RUN_TEST(test_registering_the_same_verb_object_twice_is_once);
    RUN_TEST(test_a_reply_is_up_to_the_verbs_own_handler);
    RUN_TEST(test_a_latch_is_taken_once_and_carries_its_args);
    RUN_TEST(test_a_second_set_before_a_take_keeps_the_newer_args);
    RUN_TEST(test_a_latch_truncates_args_that_do_not_fit_without_overflowing);
    RUN_TEST(test_taking_into_a_smaller_buffer_truncates_too);
    RUN_TEST(test_dispatch_or_latch_prefers_a_registered_verb_over_latching);
    RUN_TEST(test_dispatch_or_latch_latches_a_line_no_verb_claims);
    RUN_TEST(test_app_line_offer_reaches_a_running_apps_callback);
    RUN_TEST(test_app_line_offer_is_false_without_an_app_a_callback_or_a_claim);
    RUN_TEST(test_touch_line_carries_its_sample);
    RUN_TEST(test_a_malformed_touch_line_changes_nothing);
    RUN_TEST(test_a_touch_line_fits_the_console_line);
    RUN_TEST(test_an_imu_line_carries_its_sample);
    RUN_TEST(test_a_malformed_imu_line_changes_nothing);
}

SUITE_REGISTER(suite_console)

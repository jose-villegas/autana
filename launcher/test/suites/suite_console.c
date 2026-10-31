/*
 * Portable suite: console_verbs (dispatch, registration discipline,
 * console_word_match(), console_find_clash(), the line assembler) and
 * console_frame_request (the frame-loop handoff), driven here with a
 * registry and slots this suite owns, never console_shared() or
 * console_frame_slots(), which only a device build's verbs ever touch. Also the input-injection and
 * app-name parsers (console_inject_parse.h, console_navigation_parse.h),
 * pure enough to run on a host.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "testkit/cleanup.h"
#include "unity.h"

#include "console/console_frame_request.h"
#include "console/console_verbs.h"
#include "input/console/console_inject_parse.h"
#include "profile/console/console_perf_parse.h"
#include "shell/console/console_navigation_parse.h"
#include "sound/console/console_sound_parse.h"

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
    alpha_verb = (console_verb_t){"ALPHA", handle_alpha, NULL, CONSOLE_LINE_MAX};
    beta_verb = (console_verb_t){"BETA", handle_beta, NULL, CONSOLE_LINE_MAX};
    set_verb = (console_verb_t){"SET", handle_set, NULL, CONSOLE_LINE_MAX};
    settle_verb = (console_verb_t){"SETTLE", handle_settle, NULL, CONSOLE_LINE_MAX};
    tune_verb = (console_verb_t){"TUNE", handle_tune, NULL, CONSOLE_LINE_MAX};
    tunes_verb = (console_verb_t){"TUNES", handle_tunes, NULL, CONSOLE_LINE_MAX};
    lower_set_verb = (console_verb_t){"set", handle_alpha, NULL, CONSOLE_LINE_MAX};
}

static void
test_overlong_short_verb_is_dropped(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    char line[CONSOLE_LINE_MAX + 1];
    memset(line, 'x', sizeof line - 1);
    memcpy(line, "ALPHA ", strlen("ALPHA "));
    line[sizeof line - 1] = '\0';
    TEST_ASSERT_FALSE(console_registry_dispatch(&registry, line, collect) == CONSOLE_LINE_HANDLED);
    TEST_ASSERT_EQUAL_STRING("", last_verb);
}

static void
test_registered_long_verbs_own_their_bounds(void) {
    fixture();
    const char* names[] = {"sound", "runsuite", "OTHER"};
    TEST_ASSERT_EQUAL_INT(CONSOLE_LINE_MAX, console_registry_line_max(&registry));
    char line[CONSOLE_SOUND_LINE_MAX + 1];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        console_registry_t verbs = {0};
        console_verb_t verb = {names[i], handle_alpha, NULL, CONSOLE_SOUND_LINE_MAX};
        TEST_ASSERT_TRUE(console_register(&verbs, &verb));
        TEST_ASSERT_EQUAL_INT(CONSOLE_SOUND_LINE_MAX, console_registry_line_max(&verbs));
        memset(line, 'x', CONSOLE_SOUND_LINE_MAX);
        memcpy(line, names[i], strlen(names[i]));
        line[strlen(names[i])] = ' ';
        line[CONSOLE_SOUND_LINE_MAX - 1] = '\0';
        TEST_ASSERT_TRUE(console_registry_dispatch(&verbs, line, collect) == CONSOLE_LINE_HANDLED);
        line[CONSOLE_SOUND_LINE_MAX - 1] = 'x';
        line[CONSOLE_SOUND_LINE_MAX] = '\0';
        TEST_ASSERT_FALSE(console_registry_dispatch(&verbs, line, collect) == CONSOLE_LINE_HANDLED);
        TEST_ASSERT_EQUAL_INT(CONSOLE_LINE_REJECTED, console_registry_dispatch(&registry, line, collect));
    }
}

static bool
say(const char* line) {
    reply_count = 0;
    return console_registry_dispatch(&registry, line, collect) == CONSOLE_LINE_HANDLED;
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

/* SET must not answer to SETTLE, and TUNE must not answer to TUNES: the
 * same pair suite_tune.c already pins one level up (console/console_tune.c). Proven
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
 * clash console_register() already refuses; otherwise it would register
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
test_listing_orders_mixed_case_names_folded(void) {
    fixture();
    console_verb_t first = {.name = "alpha"}, last = {.name = "BETA"};
    TEST_ASSERT_TRUE(console_register(&registry, &first));
    TEST_ASSERT_TRUE(console_register(&registry, &last));
    TEST_ASSERT_EQUAL_PTR(&first, registry.first);
    TEST_ASSERT_EQUAL_PTR(&last, first.next);
    TEST_ASSERT_NULL(last.next);
    registry.first = NULL;
}

static void
test_a_name_declared_twice_stays_with_the_first(void) {
    fixture();
    static console_verb_t usurper;
    usurper = (console_verb_t){"ALPHA", handle_beta, NULL, CONSOLE_LINE_MAX};
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

/* A slot's payload in these tests: one line of text. */
typedef struct {
    char text[CONSOLE_LINE_MAX];
} text_request_t;

/* Per test, off the stack: payloads are a few hundred bytes, and the suite
 * also runs on the board's frame-loop task. */
typedef struct {
    console_frame_slots_t slots;
    console_frame_slot_t latest, held, answered;
    text_request_t storage[3][2]; /* each slot's pending and taken */
    text_request_t request;       /* a post's; the racing writer's */
    char answer[CONSOLE_LINE_MAX];
    int answer_count;
    bool posted_while_answering;
    atomic_bool writing;
} frame_fixture_t;

static frame_fixture_t* frame;

static void
free_frame(void) {
    free(frame);
    frame = NULL;
}

/* Also posts to its own slot, which a held slot refuses while answering. */
static void
record_answer(const void* payload) {
    const text_request_t* request = payload;
    console_copy_text(frame->answer, sizeof frame->answer, request->text);
    frame->answer_count++;
    frame->posted_while_answering = console_frame_post(&frame->slots, &frame->answered, request);
}

static console_frame_slot_t
slot_over(const char* name, text_request_t storage[2], bool held, void (*answer)(const void*)) {
    return (console_frame_slot_t){.name = name,
                                  .pending = &storage[0],
                                  .taken = &storage[1],
                                  .size = sizeof(text_request_t),
                                  .answer = answer,
                                  .held = held};
}

static frame_fixture_t*
frame_fixture(void) {
    frame = malloc(sizeof *frame);
    TEST_ASSERT_NOT_NULL(frame);
    *frame = (frame_fixture_t){.slots = CONSOLE_FRAME_SLOTS_INIT};
    frame->latest = slot_over("latest", frame->storage[0], false, NULL);
    frame->held = slot_over("held", frame->storage[1], true, NULL);
    frame->answered = slot_over("answered", frame->storage[2], true, record_answer);
    TEST_ASSERT_TRUE(console_frame_add(&frame->slots, &frame->latest));
    TEST_ASSERT_TRUE(console_frame_add(&frame->slots, &frame->held));
    TEST_ASSERT_TRUE(console_frame_add(&frame->slots, &frame->answered));
    suite_set_test_cleanup(free_frame);
    return frame;
}

static bool
post(frame_fixture_t* f, console_frame_slot_t* slot, const char* text) {
    console_copy_text(f->request.text, sizeof f->request.text, text);
    return console_frame_post(&f->slots, slot, &f->request);
}

static const char*
taken_text(const console_frame_slot_t* slot) {
    return ((const text_request_t*)slot->taken)->text;
}

static void
test_a_request_is_taken_once_with_its_payload(void) {
    frame_fixture_t* f = frame_fixture();

    TEST_ASSERT_FALSE(console_frame_take(&f->slots, &f->latest));
    TEST_ASSERT_TRUE(post(f, &f->latest, "sand"));
    TEST_ASSERT_TRUE(console_frame_take(&f->slots, &f->latest));
    TEST_ASSERT_EQUAL_STRING("sand", taken_text(&f->latest));
    TEST_ASSERT_FALSE(console_frame_take(&f->slots, &f->latest));
}

static void
test_a_later_request_replaces_its_own_slot_and_leaves_the_others(void) {
    frame_fixture_t* f = frame_fixture();

    TEST_ASSERT_TRUE(post(f, &f->latest, "first"));
    TEST_ASSERT_TRUE(post(f, &f->held, "console"));
    TEST_ASSERT_TRUE(post(f, &f->latest, "sand"));

    TEST_ASSERT_TRUE(console_frame_take(&f->slots, &f->latest));
    TEST_ASSERT_EQUAL_STRING("sand", taken_text(&f->latest));
    TEST_ASSERT_TRUE(console_frame_take(&f->slots, &f->held));
    TEST_ASSERT_EQUAL_STRING("console", taken_text(&f->held));
}

/* A held slot (RUNSUITE, SOUND): one at a time, from its post until the
 * frame loop says it is done, and a second is refused rather than
 * replacing it. */
static void
test_a_held_request_refuses_another_until_it_is_done(void) {
    frame_fixture_t* f = frame_fixture();

    TEST_ASSERT_TRUE(post(f, &f->held, "console"));
    TEST_ASSERT_FALSE(post(f, &f->held, "gfx"));
    TEST_ASSERT_TRUE(console_frame_take(&f->slots, &f->held));
    TEST_ASSERT_EQUAL_STRING("console", taken_text(&f->held));
    TEST_ASSERT_FALSE(post(f, &f->held, "gfx"));
    TEST_ASSERT_FALSE(console_frame_take(&f->slots, &f->held));

    console_frame_done(&f->slots, &f->held);
    TEST_ASSERT_TRUE(post(f, &f->held, "gfx"));
}

/* Only a slot with an answer is answered, once per post, and a held one
 * accepts a post again once answered. */
static void
test_pending_answers_run_once_and_free_a_held_slot(void) {
    frame_fixture_t* f = frame_fixture();

    console_frame_answer_pending(&f->slots);
    TEST_ASSERT_EQUAL_INT(0, f->answer_count);

    TEST_ASSERT_TRUE(post(f, &f->answered, "play"));
    TEST_ASSERT_TRUE(post(f, &f->latest, "sand"));
    console_frame_answer_pending(&f->slots);
    TEST_ASSERT_EQUAL_INT(1, f->answer_count);
    TEST_ASSERT_EQUAL_STRING("play", f->answer);
    TEST_ASSERT_FALSE(f->posted_while_answering);
    console_frame_answer_pending(&f->slots);
    TEST_ASSERT_EQUAL_INT(1, f->answer_count);

    TEST_ASSERT_TRUE(post(f, &f->answered, "again"));
    TEST_ASSERT_TRUE(console_frame_take(&f->slots, &f->latest));
}

/* FRAMEWATCH and SCREENSHOT: no payload, the post itself is the request. */
static void
test_a_signal_is_taken_once_per_post(void) {
    frame_fixture_t* f = frame_fixture();
    console_frame_slot_t signal = {.name = "signal"};
    TEST_ASSERT_TRUE(console_frame_add(&f->slots, &signal));

    TEST_ASSERT_FALSE(console_frame_take(&f->slots, &signal));
    TEST_ASSERT_TRUE(console_frame_post(&f->slots, &signal, NULL));
    TEST_ASSERT_TRUE(console_frame_post(&f->slots, &signal, NULL));
    TEST_ASSERT_TRUE(console_frame_take(&f->slots, &signal));
    TEST_ASSERT_FALSE(console_frame_take(&f->slots, &signal));
}

static void
test_a_slot_name_is_taken_once(void) {
    frame_fixture_t* f = frame_fixture();
    console_frame_slot_t twin = slot_over("latest", f->storage[0], false, NULL);

    TEST_ASSERT_FALSE(console_frame_add(&f->slots, &twin));
    TEST_ASSERT_TRUE(console_frame_add(&f->slots, &f->latest));
}

/* How many requests the racing writer posts. */
#define RACE_POSTS 20000

/* All one letter, alternating posts: a take that mixes letters saw a post
 * half written. */
static void*
race_writer(void* context) {
    frame_fixture_t* f = context;
    for (int i = 0; i < RACE_POSTS; i++) {
        memset(f->request.text, (i % 2) == 0 ? 'o' : 'h', sizeof f->request.text - 1);
        f->request.text[sizeof f->request.text - 1] = '\0';
        (void)console_frame_post(&f->slots, &f->latest, &f->request);
    }
    atomic_store(&f->writing, false);
    return NULL;
}

static bool
taken_whole(const char* text) {
    for (size_t i = 0; i < CONSOLE_LINE_MAX - 1; i++) {
        if (text[i] != text[0]) {
            return false;
        }
    }
    return text[CONSOLE_LINE_MAX - 1] == '\0';
}

static void
test_a_take_never_sees_a_post_half_written(void) {
    frame_fixture_t* f = frame_fixture();
    atomic_init(&f->writing, true);
    pthread_t writer;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&writer, NULL, race_writer, f));

    int takes = 0;
    int torn = 0;
    /* One take after the writer is seen done, so its last post counts even
     * when it finished first, as it may on the board, where its priority
     * outranks this task's: the host run is the one sure to race. */
    bool writing;
    do {
        writing = atomic_load(&f->writing);
        if (console_frame_take(&f->slots, &f->latest)) {
            takes++;
            torn += taken_whole(taken_text(&f->latest)) ? 0 : 1;
        }
    } while (writing);
    (void)pthread_join(writer, NULL);
    TEST_ASSERT_GREATER_THAN_INT(0, takes);
    TEST_ASSERT_EQUAL_INT(0, torn);
}

/* console_word_match() is console_registry_dispatch() == CONSOLE_LINE_HANDLED's own matcher,
 * exported so the shell can match an app's console prefix the same way. */
static void
test_word_match_carries_what_follows_the_name(void) {
    const char* args;

    TEST_ASSERT_TRUE(console_word_match("example status", "example", &args));
    TEST_ASSERT_EQUAL_STRING("status", args);

    TEST_ASSERT_TRUE(console_word_match("example", "example", &args));
    TEST_ASSERT_EQUAL_STRING("", args);

    TEST_ASSERT_FALSE(console_word_match("examples", "example", &args));
    TEST_ASSERT_FALSE(console_word_match("exa", "example", &args));
}

static void
test_word_match_folds_case(void) {
    const char* args;
    TEST_ASSERT_TRUE(console_word_match("EXAMPLE status", "example", &args));
    TEST_ASSERT_EQUAL_STRING("status", args);
}

/* console_name_cmp() orders names as strcmp() does with ASCII case folded:
 * the registry's sort and the clash check both rest on it. */
static void
test_name_cmp_folds_case_and_orders_unsigned(void) {
    TEST_ASSERT_EQUAL_INT(0, console_name_cmp("Screenshot", "SCREENSHOT"));
    TEST_ASSERT_LESS_THAN_INT(0, console_name_cmp("set", "SETX"));
    TEST_ASSERT_GREATER_THAN_INT(0, console_name_cmp("SETX", "set"));
    TEST_ASSERT_LESS_THAN_INT(0, console_name_cmp("a", "B"));
    /* A byte past ASCII sorts after every letter, as an unsigned char does. */
    TEST_ASSERT_GREATER_THAN_INT(0, console_name_cmp("\xE9", "z"));
}

/* console_find_clash() is the boot-time clash check's own primitive
 * (shell/shell.c): a verb's name and an app's prefix, or two apps' own prefixes,
 * must never fold to the same word; a prefix must carry no space of its
 * own and fit CONSOLE_LINE_MAX. */
static void
test_find_clash_finds_an_app_prefix_that_folds_to_a_registered_verb(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    const char* prefixes[] = {"alpha"};
    const char* from = NULL;
    const char* other = NULL;

    TEST_ASSERT_EQUAL_INT(CONSOLE_CLASH_VERB, console_find_clash(&registry, prefixes, 1, &from, &other));
    TEST_ASSERT_EQUAL_STRING("alpha", from);
    TEST_ASSERT_EQUAL_STRING("ALPHA", other);
}

static void
test_find_clash_finds_two_app_prefixes_that_fold_together(void) {
    fixture();
    const char* prefixes[] = {"widget", "Widget"};
    const char* from = NULL;
    const char* other = NULL;

    TEST_ASSERT_EQUAL_INT(CONSOLE_CLASH_APP, console_find_clash(&registry, prefixes, 2, &from, &other));
    TEST_ASSERT_EQUAL_STRING("Widget", from);
    TEST_ASSERT_EQUAL_STRING("widget", other);
}

static void
test_find_clash_is_none_over_a_clean_set(void) {
    fixture();
    TEST_ASSERT_TRUE(console_register(&registry, &alpha_verb));
    const char* prefixes[] = {"widget", "gadget"};
    const char* from = NULL;
    const char* other = NULL;

    TEST_ASSERT_EQUAL_INT(CONSOLE_CLASH_NONE, console_find_clash(&registry, prefixes, 2, &from, &other));
}

static void
test_find_clash_rejects_a_prefix_with_a_space(void) {
    fixture();
    const char* prefixes[] = {"set tool"};
    const char* from = NULL;
    const char* other = NULL;

    TEST_ASSERT_EQUAL_INT(CONSOLE_CLASH_SPACE, console_find_clash(&registry, prefixes, 1, &from, &other));
    TEST_ASSERT_EQUAL_STRING("set tool", from);
}

static void
test_find_clash_rejects_a_prefix_too_long_for_console_line_max(void) {
    fixture();
    char long_prefix[CONSOLE_LINE_MAX + 1];
    memset(long_prefix, 'x', sizeof long_prefix - 1);
    long_prefix[sizeof long_prefix - 1] = '\0';
    const char* prefixes[] = {long_prefix};
    const char* from = NULL;
    const char* other = NULL;

    TEST_ASSERT_EQUAL_INT(CONSOLE_CLASH_LENGTH, console_find_clash(&registry, prefixes, 1, &from, &other));
}

/* console_append_char() with the CONSOLE_LINE_MAX its own doc comment
 * describes: an over-long line must not leak a spurious short line made of
 * its own tail. */
static void
test_append_char_delivers_one_line_at_a_time(void) {
    char line[CONSOLE_LINE_MAX];
    int len = 0;
    bool overflowed = false;

    TEST_ASSERT_FALSE(console_append_char(line, &len, &overflowed, 'a'));
    TEST_ASSERT_FALSE(console_append_char(line, &len, &overflowed, 'b'));
    TEST_ASSERT_TRUE(console_append_char(line, &len, &overflowed, '\n'));
    TEST_ASSERT_EQUAL_STRING("ab", line);
}

static void
test_append_char_ignores_a_bare_terminator(void) {
    char line[CONSOLE_LINE_MAX];
    int len = 0;
    bool overflowed = false;

    TEST_ASSERT_FALSE(console_append_char(line, &len, &overflowed, '\n'));
    TEST_ASSERT_FALSE(console_append_char(line, &len, &overflowed, '\r'));
}

static void
test_append_char_discards_an_overflowing_line_and_its_terminator(void) {
    char line[CONSOLE_LINE_MAX];
    int len = 0;
    bool overflowed = false;

    for (int i = 0; i < CONSOLE_LINE_MAX + 10; i++) {
        TEST_ASSERT_FALSE_MESSAGE(console_append_char(line, &len, &overflowed, 'x'), "no line completes mid-overflow");
    }
    TEST_ASSERT_TRUE_MESSAGE(overflowed, "a line past CONSOLE_LINE_MAX-1 must be marked overflowed");
    TEST_ASSERT_FALSE_MESSAGE(console_append_char(line, &len, &overflowed, '\n'),
                              "the overflowing line's own terminator must not complete it either");
    TEST_ASSERT_FALSE_MESSAGE(overflowed, "the next line starts clean");
}

/* An over-long line's tail is never delivered as a line of its own. */
static void
test_append_char_the_tail_after_an_overflow_is_not_a_line_of_its_own(void) {
    char line[CONSOLE_LINE_MAX];
    int len = 0;
    bool overflowed = false;

    for (int i = 0; i < CONSOLE_LINE_MAX - 1; i++) {
        console_append_char(line, &len, &overflowed, 'x');
    }
    console_append_char(line, &len, &overflowed, 'x'); /* this one overflows it */

    TEST_ASSERT_FALSE(console_append_char(line, &len, &overflowed, 'o'));
    TEST_ASSERT_FALSE(console_append_char(line, &len, &overflowed, 'k'));
    TEST_ASSERT_FALSE_MESSAGE(console_append_char(line, &len, &overflowed, '\n'),
                              "'ok' is the overflowing line's own tail, never a line of its own");
}

static void
test_touch_line_carries_its_sample(void) {
    bool down = false;
    vec2i_t point = {0};

    TEST_ASSERT_TRUE(console_touch_parse("down 184 224", &down, &point));
    TEST_ASSERT_TRUE(down);
    TEST_ASSERT_EQUAL_INT(184, point.x);
    TEST_ASSERT_EQUAL_INT(224, point.y);

    TEST_ASSERT_TRUE(console_touch_parse("up 7 9", &down, &point));
    TEST_ASSERT_FALSE(down);
    TEST_ASSERT_EQUAL_INT(7, point.x);
    TEST_ASSERT_EQUAL_INT(9, point.y);
}

/* A half-read sample is worse than none: the caller would move a finger
 * somewhere nobody asked for. */
static void
test_a_malformed_touch_line_changes_nothing(void) {
    bool down = true;
    vec2i_t point = {11, 22};

    TEST_ASSERT_FALSE(console_touch_parse("left 1 2", &down, &point));
    TEST_ASSERT_FALSE(console_touch_parse("down 1", &down, &point));
    TEST_ASSERT_FALSE(console_touch_parse(" ", &down, &point));
    TEST_ASSERT_FALSE(console_touch_parse("down 1 2 junk", &down, &point));
    TEST_ASSERT_FALSE(console_touch_parse("down 1 2junk", &down, &point));
    TEST_ASSERT_TRUE(down);
    TEST_ASSERT_EQUAL_INT(11, point.x);
    TEST_ASSERT_EQUAL_INT(22, point.y);
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
    TEST_ASSERT_EQUAL_INT(IMU_COUNTS_PER_G, ax);
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

static void
test_a_tap_line_uses_the_short_default_duration(void) {
    console_touch_gesture_t gesture;

    TEST_ASSERT_TRUE(console_tap_parse("184 224", &gesture));
    TEST_ASSERT_EQUAL_INT(184, gesture.from.x);
    TEST_ASSERT_EQUAL_INT(224, gesture.from.y);
    TEST_ASSERT_EQUAL_UINT32(CONSOLE_TAP_DEFAULT_MS, gesture.ms);
}

static void
test_a_tap_line_rejects_a_duration(void) {
    console_touch_gesture_t gesture = {.from = {7, 0}, .ms = 9};

    TEST_ASSERT_FALSE(console_tap_parse("10 20 5000", &gesture));
    TEST_ASSERT_EQUAL_INT(7, gesture.from.x);
    TEST_ASSERT_EQUAL_UINT32(9, gesture.ms);
}

static void
test_a_drag_line_carries_both_endpoints_and_duration(void) {
    console_touch_gesture_t gesture;

    TEST_ASSERT_TRUE(console_drag_parse("1 2 300 400 250", &gesture));
    TEST_ASSERT_EQUAL_INT(1, gesture.from.x);
    TEST_ASSERT_EQUAL_INT(2, gesture.from.y);
    TEST_ASSERT_EQUAL_INT(300, gesture.to.x);
    TEST_ASSERT_EQUAL_INT(400, gesture.to.y);
    TEST_ASSERT_EQUAL_UINT32(250, gesture.ms);
}

static void
test_a_bad_gesture_line_changes_nothing(void) {
    console_touch_gesture_t gesture = {.from = {7, 0}, .ms = 9};

    TEST_ASSERT_FALSE(console_press_parse("1 2 0", &gesture));
    TEST_ASSERT_FALSE(console_drag_parse("1 2 3", &gesture));
    TEST_ASSERT_EQUAL_INT(7, gesture.from.x);
    TEST_ASSERT_EQUAL_UINT32(9, gesture.ms);
}

static void
test_a_button_line_carries_its_kind(void) {
    console_button_t button;
    bool held = false;

    TEST_ASSERT_TRUE(console_button_parse("boot long", &button, &held));
    TEST_ASSERT_EQUAL_INT(CONSOLE_BUTTON_BOOT, button);
    TEST_ASSERT_TRUE(held);

    TEST_ASSERT_TRUE(console_button_parse("power", &button, &held));
    TEST_ASSERT_EQUAL_INT(CONSOLE_BUTTON_POWER, button);
    TEST_ASSERT_FALSE(held);
}

static void
test_an_app_name_accepts_a_case_folded_prefix(void) {
    TEST_ASSERT_TRUE(console_app_name_matches("Star Chart", "star"));
    TEST_ASSERT_TRUE(console_app_name_matches("Star Chart", "STAR CHART"));
    TEST_ASSERT_FALSE(console_app_name_matches("Star Chart", "moon"));
    TEST_ASSERT_FALSE(console_app_name_matches("Star Chart", "starry"));
    /* The word a person reaches for is rarely the first one. */
    TEST_ASSERT_TRUE(console_app_name_matches("Star Chart", "chart"));
    TEST_ASSERT_TRUE(console_app_name_matches("Star Chart", "CHART"));
    TEST_ASSERT_FALSE(console_app_name_matches("Star Chart", "hart"));
    TEST_ASSERT_FALSE(console_app_name_matches("Star Chart", ""));
    TEST_ASSERT_FALSE(console_app_name_matches("Star Chart", ""));
}

static const char* const PERF_NAMES[] = {"stage.a", "stage.b"};
static const char* const PERF_EVENTS[] = {"insn", "window"};
static int perf_posted_name;
static int perf_posted_event;
static int perf_posts;
static int perf_dropped;
static char perf_lines[6][112];
static int perf_line_count;

static void
perf_collect(const char* line) {
    if (perf_line_count < 6) {
        snprintf(perf_lines[perf_line_count], sizeof perf_lines[0], "%s", line);
    }
    perf_line_count++;
}

static int
perf_name_index(const char* name) {
    for (int i = 0; i < 2; i++) {
        if (strcmp(PERF_NAMES[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static int
perf_event_index(const char* event) {
    for (int i = 0; i < 2; i++) {
        if (strcmp(PERF_EVENTS[i], event) == 0) {
            return i;
        }
    }
    return -1;
}

static const char*
perf_name_at(int index) {
    return index >= 0 && index < 2 ? PERF_NAMES[index] : NULL;
}

static const char*
perf_event_at(int index) {
    return index >= 0 && index < 2 ? PERF_EVENTS[index] : NULL;
}

static int
perf_names_dropped(void) {
    return perf_dropped;
}

static void
perf_post(int name_index, int event_index) {
    perf_posted_name = name_index;
    perf_posted_event = event_index;
    perf_posts++;
}

static void
perf_run(const char* args) {
    static const console_perf_ops_t ops = {perf_name_index, perf_event_index,   perf_name_at,
                                           perf_event_at,   perf_names_dropped, perf_post};
    perf_line_count = 0;
    perf_posts = 0;
    perf_dropped = 0;
    console_perf_dispatch(args, &ops, perf_collect);
}

static void
test_perf_with_no_words_or_a_question_mark_lists_names_and_events(void) {
    const char* const forms[] = {"", "?", "  ?  "};
    for (int i = 0; i < 3; i++) {
        perf_run(forms[i]);
        TEST_ASSERT_EQUAL_INT(5, perf_line_count);
        TEST_ASSERT_EQUAL_STRING("PERFMON_NAME stage.a", perf_lines[0]);
        TEST_ASSERT_EQUAL_STRING("PERFMON_EVENT window", perf_lines[3]);
        TEST_ASSERT_EQUAL_STRING("PERFMON_END", perf_lines[4]);
        TEST_ASSERT_EQUAL_INT(0, perf_posts);
    }
}

static void
test_perf_listing_reports_names_that_did_not_fit(void) {
    perf_dropped = 3;
    static const console_perf_ops_t ops = {perf_name_index, perf_event_index,   perf_name_at,
                                           perf_event_at,   perf_names_dropped, perf_post};
    perf_line_count = 0;
    console_perf_dispatch("?", &ops, perf_collect);
    TEST_ASSERT_EQUAL_STRING("PERFMON_NAMES_DROPPED 3", perf_lines[2]);
}

static void
test_perf_off_disarms(void) {
    perf_run("off");
    TEST_ASSERT_EQUAL_INT(1, perf_posts);
    TEST_ASSERT_EQUAL_INT(-1, perf_posted_name);
    TEST_ASSERT_EQUAL_STRING("PERFMON_OK off", perf_lines[0]);
}

static void
test_perf_arms_a_known_name_with_the_first_event_unless_one_is_named(void) {
    perf_run("stage.b");
    TEST_ASSERT_EQUAL_INT(1, perf_posted_name);
    TEST_ASSERT_EQUAL_INT(0, perf_posted_event);
    TEST_ASSERT_EQUAL_STRING("PERFMON_OK stage.b insn", perf_lines[0]);

    perf_run("stage.a window");
    TEST_ASSERT_EQUAL_INT(0, perf_posted_name);
    TEST_ASSERT_EQUAL_INT(1, perf_posted_event);
    TEST_ASSERT_EQUAL_STRING("PERFMON_OK stage.a window", perf_lines[0]);
}

static void
test_perf_refuses_an_unknown_name_or_event_and_posts_nothing(void) {
    perf_run("nope");
    TEST_ASSERT_EQUAL_STRING("PERFMON_ERR unknown name nope", perf_lines[0]);
    perf_run("stage.a nope");
    TEST_ASSERT_EQUAL_STRING("PERFMON_ERR unknown event nope", perf_lines[0]);
    TEST_ASSERT_EQUAL_INT(0, perf_posts);
}

static void
test_perf_with_a_third_word_is_a_usage_error(void) {
    perf_run("stage.a insn extra");
    TEST_ASSERT_EQUAL_INT(1, perf_line_count);
    TEST_ASSERT_EQUAL_STRING("PERFMON_ERR usage: PERF <name|off|?> [event]", perf_lines[0]);
    TEST_ASSERT_EQUAL_INT(0, perf_posts);
}

static void
test_perf_keeps_a_long_word_whole_rather_than_splitting_it_into_name_and_event(void) {
    perf_run("a_name_far_longer_than_twenty_four_chars");
    TEST_ASSERT_EQUAL_INT(1, perf_line_count);
    TEST_ASSERT_NOT_NULL(strstr(perf_lines[0], "unknown name a_name_far_longer_than_twenty_four"));

    perf_run("stage.a an_event_name_far_longer_than_seventeen");
    TEST_ASSERT_NOT_NULL(strstr(perf_lines[0], "unknown event an_event_name_far_longer"));
    TEST_ASSERT_EQUAL_INT(0, perf_posts);
}

static void
test_quarter_injection_matches_forward_mapping(void) {
    fixture();
    for (int quarter = 0; quarter < DISPLAY_QUARTER_COUNT; quarter++) {
        char args[CONSOLE_LINE_MAX];
        snprintf(args, sizeof args, "quarter %d", quarter);
        imu_sample_t sample;
        int parsed_quarter = -1;
        TEST_ASSERT_TRUE(console_imu_quarter_parse(args, &sample, &parsed_quarter));
        TEST_ASSERT_EQUAL_INT(quarter, parsed_quarter);
        display_t display;
        display_init(&display);
        const viewport_quarter_axes_t axes = viewport_quarter_axes(quarter);
        const vec2i_t gravity = imu_gravity_screen(&sample);
        TEST_ASSERT_EQUAL_INT(axes.x_down * IMU_COUNTS_PER_G, gravity.x);
        TEST_ASSERT_EQUAL_INT(axes.y_down * IMU_COUNTS_PER_G, gravity.y);
        display.quarter = quarter;
        TEST_ASSERT_FALSE(display_update(
            &display, (vec3f_t){(float)gravity.x / IMU_COUNTS_PER_G, (float)gravity.y / IMU_COUNTS_PER_G, 0.0F}, 0));
        TEST_ASSERT_EQUAL_INT(quarter, display_quarter(&display));
    }
    imu_sample_t sample = {.ax = IMU_COUNTS_PER_G};
    char past_end[CONSOLE_LINE_MAX];
    snprintf(past_end, sizeof past_end, "quarter %d", DISPLAY_QUARTER_COUNT);
    const char* invalid[] = {"quarter -1", "quarter", "quarter 0 extra", past_end};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        int parsed_quarter = -1;
        TEST_ASSERT_FALSE(console_imu_quarter_parse(invalid[i], &sample, &parsed_quarter));
        TEST_ASSERT_EQUAL_INT(-1, parsed_quarter);
        TEST_ASSERT_EQUAL_INT(IMU_COUNTS_PER_G, sample.ax);
    }
}

static void
test_sound_arguments(void) {
    console_sound_args_t args;
    TEST_ASSERT_TRUE(console_sound_parse("play engine/tap loop 25", &args));
    TEST_ASSERT_EQUAL_STRING("tap", args.name);
    TEST_ASSERT_EQUAL_STRING(ASSET_ENGINE_PACK, args.pack);
    TEST_ASSERT_TRUE(console_sound_parse("play tap loop 25", &args));
    TEST_ASSERT_EQUAL_INT(CONSOLE_SOUND_PLAY, args.action);
    TEST_ASSERT_EQUAL_STRING("tap", args.name);
    TEST_ASSERT_TRUE(args.loop);
    TEST_ASSERT_EQUAL_INT(AUDIO_OUT_VOLUME_MAX_PCT / 4, args.gain_pct);
    TEST_ASSERT_TRUE(console_sound_parse("play tap", &args));
    TEST_ASSERT_EQUAL_INT(AUDIO_OUT_VOLUME_MAX_PCT, args.gain_pct);
    TEST_ASSERT_FALSE(args.loop);
    TEST_ASSERT_TRUE(console_sound_parse("play music/theme loop 50", &args));
    TEST_ASSERT_EQUAL_STRING("music", args.pack);
    TEST_ASSERT_EQUAL_STRING("theme", args.name);
    TEST_ASSERT_TRUE(console_sound_parse("play tap 0", &args));
    TEST_ASSERT_EQUAL_INT(0, args.gain_pct);
    TEST_ASSERT_TRUE(console_sound_parse("stop", &args));
    TEST_ASSERT_EQUAL_INT(CONSOLE_SOUND_STOP, args.action);
    TEST_ASSERT_EQUAL_UINT32(0, args.voice);
    TEST_ASSERT_TRUE(console_sound_parse("stop all", &args));
    TEST_ASSERT_TRUE(console_sound_parse("stop 123", &args));
    TEST_ASSERT_EQUAL_UINT32(123, args.voice);
    TEST_ASSERT_TRUE(console_sound_parse("stats", &args));
    TEST_ASSERT_EQUAL_INT(CONSOLE_SOUND_STATS, args.action);
    TEST_ASSERT_TRUE(console_sound_parse("onset start", &args));
    TEST_ASSERT_EQUAL_INT(CONSOLE_SOUND_ONSET, args.action);
    TEST_ASSERT_EQUAL_INT(CONSOLE_SOUND_START, args.event);
    TEST_ASSERT_EQUAL_INT(CONSOLE_SOUND_ONSET_HZ, args.hz);
    TEST_ASSERT_EQUAL_INT(CONSOLE_SOUND_ONSET_GAIN, args.gain_pct);
    TEST_ASSERT_TRUE(console_sound_parse("onset amp-off 300 0", &args));
    TEST_ASSERT_EQUAL_INT(CONSOLE_SOUND_AMP_OFF, args.event);
    TEST_ASSERT_EQUAL_INT(0, args.gain_pct);
    static const char* const invalid[] = {"",
                                          "onset",
                                          "onset unknown",
                                          "onset start 0",
                                          "onset stop 500 101",
                                          "onset steal 500 33 extra",
                                          "onset amp-on -1",
                                          "onset start 500 -1",
                                          "play /tap",
                                          "play engine/",
                                          "play a/b/c",
                                          "play",
                                          "play tap loop loop",
                                          "play tap 101",
                                          "play tap -1",
                                          "play tap 999999999999999999999999999",
                                          "stats extra",
                                          "stop 0",
                                          "stop 1 extra",
                                          "stop alligator",
                                          "play tap looping",
                                          "stop -1",
                                          "stop 4294967296",
                                          "play a_name_far_longer_than_the_asset_entry_name_limit",
                                          "play tap 20 extra"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        TEST_ASSERT_FALSE_MESSAGE(console_sound_parse(invalid[i], &args), invalid[i]);
    }
}

static void
test_sound_parse_boundaries_and_whitespace(void) {
    console_sound_args_t args;
    char line[CONSOLE_SOUND_LINE_MAX];
    char name[ASSET_NAME_MAX + 1];
    memset(name, 'x', ASSET_NAME_MAX);
    name[ASSET_NAME_MAX - 1] = '\0';
    (void)snprintf(line, sizeof line, "play %s loop 0", name);
    TEST_ASSERT_TRUE(console_sound_parse(line, &args));
    name[ASSET_NAME_MAX - 1] = 'x';
    name[ASSET_NAME_MAX] = '\0';
    (void)snprintf(line, sizeof line, "play %s", name);
    TEST_ASSERT_FALSE(console_sound_parse(line, &args));
    TEST_ASSERT_TRUE(console_sound_parse(" \tplay\ttap \vloop\r100\n", &args));
    TEST_ASSERT_FALSE(console_sound_parse("play tap loop 100 extra", &args));
    name[ASSET_NAME_MAX - 1] = '\0';
    (void)snprintf(line, sizeof line, "play %s/%s loop 100", name, name);
    TEST_ASSERT_TRUE(console_sound_parse(line, &args));
    TEST_ASSERT_EQUAL_STRING(name, args.pack);
    TEST_ASSERT_EQUAL_STRING(name, args.name);
    name[ASSET_NAME_MAX - 1] = 'x';
    (void)snprintf(line, sizeof line, "play %s/tap", name);
    TEST_ASSERT_FALSE(console_sound_parse(line, &args));
    TEST_ASSERT_TRUE(console_sound_parse("stop 4294967295", &args));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, args.voice);
}

static void
test_imu_reply_exposes_judge_timing_and_checks_capacity(void) {
    char reply[CONSOLE_IMU_REPLY_SIZE];
    TEST_ASSERT_TRUE(console_imu_reply(reply, sizeof reply, DISPLAY_DEFAULT_QUARTER));
    int quarter, count, sample_ms, commit_ms;
    TEST_ASSERT_EQUAL_INT(CONSOLE_IMU_REPLY_INTEGER_COUNT,
                          sscanf(reply, CONSOLE_IMU_REPLY_FORMAT, &quarter, &count, &sample_ms, &commit_ms));
    TEST_ASSERT_EQUAL_INT(DISPLAY_DEFAULT_QUARTER, quarter);
    TEST_ASSERT_EQUAL_INT(DISPLAY_QUARTER_COUNT, count);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SAMPLE_MS, sample_ms);
    TEST_ASSERT_EQUAL_INT(DISPLAY_COMMIT_MS, commit_ms);
    TEST_ASSERT_FALSE(console_imu_reply(reply, strlen(reply), DISPLAY_DEFAULT_QUARTER));
}

void
suite_console(void) {
    RUN_TEST(test_imu_reply_exposes_judge_timing_and_checks_capacity);
    RUN_TEST(test_sound_parse_boundaries_and_whitespace);
    RUN_TEST(test_sound_arguments);
    RUN_TEST(test_overlong_short_verb_is_dropped);
    RUN_TEST(test_registered_long_verbs_own_their_bounds);
    RUN_TEST(test_exact_name_matches);
    RUN_TEST(test_name_plus_space_carries_args_without_the_verb_or_the_space);
    RUN_TEST(test_a_verb_does_not_swallow_a_longer_word_that_starts_with_its_name);
    RUN_TEST(test_a_verb_answers_whatever_case_it_is_typed_in);
    RUN_TEST(test_a_name_already_taken_in_another_case_is_refused);
    RUN_TEST(test_the_longer_word_rule_holds_across_case);
    RUN_TEST(test_an_unknown_line_is_left_alone);
    RUN_TEST(test_registration_order_does_not_change_dispatch_or_listing_order);
    RUN_TEST(test_listing_orders_mixed_case_names_folded);
    RUN_TEST(test_a_name_declared_twice_stays_with_the_first);
    RUN_TEST(test_registering_the_same_verb_object_twice_is_once);
    RUN_TEST(test_a_reply_is_up_to_the_verbs_own_handler);
    RUN_TEST(test_a_request_is_taken_once_with_its_payload);
    RUN_TEST(test_a_later_request_replaces_its_own_slot_and_leaves_the_others);
    RUN_TEST(test_a_held_request_refuses_another_until_it_is_done);
    RUN_TEST(test_pending_answers_run_once_and_free_a_held_slot);
    RUN_TEST(test_a_signal_is_taken_once_per_post);
    RUN_TEST(test_a_slot_name_is_taken_once);
    RUN_TEST(test_a_take_never_sees_a_post_half_written);
    RUN_TEST(test_word_match_carries_what_follows_the_name);
    RUN_TEST(test_word_match_folds_case);
    RUN_TEST(test_name_cmp_folds_case_and_orders_unsigned);
    RUN_TEST(test_find_clash_finds_an_app_prefix_that_folds_to_a_registered_verb);
    RUN_TEST(test_find_clash_finds_two_app_prefixes_that_fold_together);
    RUN_TEST(test_find_clash_is_none_over_a_clean_set);
    RUN_TEST(test_find_clash_rejects_a_prefix_with_a_space);
    RUN_TEST(test_find_clash_rejects_a_prefix_too_long_for_console_line_max);
    RUN_TEST(test_append_char_delivers_one_line_at_a_time);
    RUN_TEST(test_append_char_ignores_a_bare_terminator);
    RUN_TEST(test_append_char_discards_an_overflowing_line_and_its_terminator);
    RUN_TEST(test_append_char_the_tail_after_an_overflow_is_not_a_line_of_its_own);
    RUN_TEST(test_touch_line_carries_its_sample);
    RUN_TEST(test_a_malformed_touch_line_changes_nothing);
    RUN_TEST(test_a_touch_line_fits_the_console_line);
    RUN_TEST(test_quarter_injection_matches_forward_mapping);
    RUN_TEST(test_an_imu_line_carries_its_sample);
    RUN_TEST(test_a_malformed_imu_line_changes_nothing);
    RUN_TEST(test_a_tap_line_uses_the_short_default_duration);
    RUN_TEST(test_a_tap_line_rejects_a_duration);
    RUN_TEST(test_a_drag_line_carries_both_endpoints_and_duration);
    RUN_TEST(test_a_bad_gesture_line_changes_nothing);
    RUN_TEST(test_a_button_line_carries_its_kind);
    RUN_TEST(test_an_app_name_accepts_a_case_folded_prefix);
    RUN_TEST(test_perf_with_no_words_or_a_question_mark_lists_names_and_events);
    RUN_TEST(test_perf_listing_reports_names_that_did_not_fit);
    RUN_TEST(test_perf_off_disarms);
    RUN_TEST(test_perf_arms_a_known_name_with_the_first_event_unless_one_is_named);
    RUN_TEST(test_perf_refuses_an_unknown_name_or_event_and_posts_nothing);
    RUN_TEST(test_perf_with_a_third_word_is_a_usage_error);
    RUN_TEST(test_perf_keeps_a_long_word_whole_rather_than_splitting_it_into_name_and_event);
}

SUITE_REGISTER(suite_console)

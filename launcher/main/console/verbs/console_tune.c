/*
 * console_tune (SET, GET, RESET, TUNE): verbs that do nothing themselves.
 * Each reassembles its line and hands it to console/console_tune, against
 * the shared registry. Development builds only; see console.h.
 */
#include "console/console_tune.h"
#include "console/console.h"
#include "console/console_verbs.h"

#include <stdio.h>

static void
forward_to_tune(const char* verb, const char* args, console_reply_fn reply) {
    (void)reply;
    char line[CONSOLE_LINE_MAX];
    const int length =
        args[0] == '\0' ? snprintf(line, sizeof line, "%s", verb) : snprintf(line, sizeof line, "%s %s", verb, args);
    if (length < 0 || (size_t)length >= sizeof line) {
        /* A clipped command would SET some other value: refuse it. */
        console_reply_stdio(CONSOLE_TUNE_ERR " line-too-long");
        return;
    }
    (void)console_tune_line(tune_shared(), line, console_reply_stdio);
}

static void
console_verb_set(const char* args, console_reply_fn reply) {
    forward_to_tune(CONSOLE_TUNE_SET, args, reply);
}

static void
console_verb_get(const char* args, console_reply_fn reply) {
    forward_to_tune(CONSOLE_TUNE_GET, args, reply);
}

static void
console_verb_reset(const char* args, console_reply_fn reply) {
    forward_to_tune(CONSOLE_TUNE_RESET, args, reply);
}

static void
console_verb_tune(const char* args, console_reply_fn reply) {
    forward_to_tune(CONSOLE_TUNE_LIST, args, reply);
}

/* GET/RESET take a name alone (TUNE_NAME_MAX); SET also takes a value, an
 * int32_t's longest text ("-2147483648", 11 chars); bare TUNE takes
 * nothing. */
#define REGISTER_TUNE_VERB(verb, request, longest_args, handler)                                                       \
    _Static_assert(__builtin_strcmp(#verb, request) == 0, #verb " differs from " #request);                            \
    CONSOLE_VERB(verb, longest_args, handler)

REGISTER_TUNE_VERB(SET, CONSOLE_TUNE_SET, TUNE_NAME_MAX + 1 + 11, console_verb_set)
REGISTER_TUNE_VERB(GET, CONSOLE_TUNE_GET, TUNE_NAME_MAX, console_verb_get)
REGISTER_TUNE_VERB(RESET, CONSOLE_TUNE_RESET, TUNE_NAME_MAX, console_verb_reset)
REGISTER_TUNE_VERB(TUNE, CONSOLE_TUNE_LIST, 0, console_verb_tune)

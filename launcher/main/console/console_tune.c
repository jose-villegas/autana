/*
 * console_tune - SET, GET, RESET, TUNE: verbs that do nothing themselves.
 * Each reassembles its line and hands it to util/tune, which stays the one
 * place owning that protocol's registry and replies. Development builds
 * only - see console.h.
 */
#include "console/console.h"
#include "console/console_verbs.h"

#include "util/tune.h"

#include <stdio.h>

static void
forward_to_tune(const char* verb, const char* args, console_reply_fn reply) {
    (void)reply;
    char line[CONSOLE_LINE_MAX];
    if (args[0] == '\0') {
        snprintf(line, sizeof line, "%s", verb);
    } else {
        snprintf(line, sizeof line, "%s %s", verb, args);
    }
    tune_handle_line(line, console_reply_stdio);
}

static void
console_verb_set(const char* args, console_reply_fn reply) {
    forward_to_tune("SET", args, reply);
}

static void
console_verb_get(const char* args, console_reply_fn reply) {
    forward_to_tune("GET", args, reply);
}

static void
console_verb_reset(const char* args, console_reply_fn reply) {
    forward_to_tune("RESET", args, reply);
}

static void
console_verb_tune(const char* args, console_reply_fn reply) {
    forward_to_tune("TUNE", args, reply);
}

/* GET/RESET take a name alone (TUNE_NAME_MAX); SET also takes a value, an
 * int32_t's longest text ("-2147483648", 11 chars); bare TUNE takes
 * nothing. */
CONSOLE_VERB(set, TUNE_NAME_MAX + 1 + 11, console_verb_set)
CONSOLE_VERB(get, TUNE_NAME_MAX, console_verb_get)
CONSOLE_VERB(reset, TUNE_NAME_MAX, console_verb_reset)
CONSOLE_VERB(tune, 0, console_verb_tune)

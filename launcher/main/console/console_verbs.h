/*
 * console_verbs - a line off the console matched to a registered verb by
 * exact name, or by name plus one space before its arguments.
 *
 * Pure and portable: this dispatch has no task, no driver and no hardware
 * dependency of its own, so it is exercised the same way on a laptop
 * (test/suites/suite_console.c drives a registry the test owns) as on the
 * device (console.c drives the one console_shared()). A verb's own handler
 * never sees its name or the space after it - `args` is only what follows.
 *
 * CONSOLE_VERB() is how a device-only file joins the shared registry before
 * app_main() runs, the same self-registering shape as TUNE() (util/tune.h)
 * and APP_REGISTER() (app.h). Its _Static_assert is the point: a verb whose
 * name plus its longest possible args cannot fit CONSOLE_LINE_MAX would
 * silently truncate on the device and nowhere else, so that mistake is
 * caught at compile time instead.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "util/tune.h"

/* The longest line any registered verb needs today: "SET " (4) + a
 * TUNE_NAME_MAX-long tunable name + " " (1) + an int32_t's longest text
 * ("-2147483648", 11 chars) + a NUL - see console_tune.c's own
 * CONSOLE_VERB(SET, ...) call, which is what this bound is sized for. */
#define CONSOLE_LINE_MAX (4 + TUNE_NAME_MAX + 1 + 11 + 1)

typedef void (*console_reply_fn)(const char* line);

/* `args` is what follows the verb and one space, or "" for a bare verb with
 * nothing after it. */
typedef void (*console_verb_fn)(const char* args, console_reply_fn reply);

typedef struct console_verb {
    const char* name; /* "SCREENSHOT", "SET" */
    console_verb_fn handle;
    struct console_verb* next;
} console_verb_t;

typedef struct {
    console_verb_t* first;
} console_registry_t;

/* Kept in name order, so dispatch and any future listing never depend on
 * link order - the same discipline tune_register() (util/tune.c) already
 * uses. A name already taken by a different verb is refused (false); the
 * same verb object registered twice is a no-op (true, since it is already
 * there). */
bool console_register(console_registry_t* registry, console_verb_t* verb);

/* True if `line` matched a registered verb - its name exactly, or its name
 * followed by one space - whatever that verb's handler then did with it.
 * False, with no handler called, if nothing matched. A verb never swallows
 * a longer word that merely starts with its name (TUNE does not take
 * TUNES, SET does not take SETTLE): a match needs the name followed by
 * either the line's end or a space, never another letter. */
bool console_registry_handle_line(console_registry_t* registry, const char* line, console_reply_fn reply);

/* Defined in console.c: the one registry every CONSOLE_VERB() below joins,
 * and the device's own line dispatch answers from. Declared here, not in
 * console.h, so this pure header can build the macro below without pulling
 * in a driver-facing header nothing here needs. */
console_registry_t* console_shared(void);

#define CONSOLE_VERB(VERB, longest_args, function)                                                                     \
    _Static_assert(sizeof(#VERB) + 1 + (longest_args) <= CONSOLE_LINE_MAX,                                             \
                   #VERB " plus its longest args cannot fit CONSOLE_LINE_MAX");                                        \
    static console_verb_t VERB##_console_verb = {#VERB, (function), NULL};                                             \
    __attribute__((constructor)) static void VERB##_console_verb_register(void) {                                      \
        console_register(console_shared(), &VERB##_console_verb);                                                      \
    }

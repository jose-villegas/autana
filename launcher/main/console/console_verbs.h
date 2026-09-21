/*
 * console_verbs - a line off the console matched to a registered verb by
 * exact name, or by name plus one space before its arguments. Also
 * console_append_char(), the byte-by-byte line assembler the console's
 * reader task feeds this dispatch from.
 *
 * Pure and portable: none of it has a task, a driver or a hardware
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

/* Assembles one line at a time from a byte stream (console.c's reader
 * task): true once `line` holds a complete one, NUL-terminated. A line
 * longer than CONSOLE_LINE_MAX-1 sets `*overflowed` and discards every
 * byte, including its own terminator, up to the next \n or \r - resetting
 * `*len` mid-line and continuing to collect (the earlier behaviour) turns
 * an over-long line's tail into a short line of its own, which now reaches
 * a registered verb or an app as if it had been typed on purpose. */
static inline bool
console_append_char(char* line, int* len, bool* overflowed, int c) {
    /* Either terminator ends a line: monitor.sh's Enter key may send '\r'
     * or '\n', depending on platform. */
    if (c == '\n' || c == '\r') {
        if (*overflowed) {
            *overflowed = false;
            *len = 0;
            return false;
        }
        if (*len == 0) {
            return false;
        }
        line[*len] = '\0';
        *len = 0;
        return true;
    }
    if (*overflowed) {
        return false;
    }
    if (*len < CONSOLE_LINE_MAX - 1) {
        line[(*len)++] = (char)c;
    } else {
        *overflowed = true;
        *len = 0;
    }
    return false;
}

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

/* True if `line` is `name`, or `name` plus one space and more (never a
 * longer word starting the same way), folded like console_register()'s own
 * clash check. `*args` gets what follows - "" for a bare match. Shared by a
 * verb's dispatch below and an app's console prefix (app.h). */
bool console_word_match(const char* line, const char* name, const char** args);

/* True if `line` matched a registered verb, whatever its handler then did.
 * False, with no handler called, if nothing matched. */
bool console_registry_handle_line(console_registry_t* registry, const char* line, console_reply_fn reply);

/* True if `name` equals any of `others[0..count)`, folded like
 * console_word_match(); `*at` is the first match's index. For the
 * boot-time check that no two console words can ever claim one line. */
bool console_name_in(const char* name, const char* const* others, int count, int* at);

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

/*
 * console_app_line - what happens to a console line no registered verb
 * claims. Header-only, like console_latch.h: both halves are small enough
 * that a .c of their own would exist only to be linked, not to hide
 * anything, so a host test can include this header directly.
 *
 * Two callers, two functions. console.c's reader task tries the registry
 * first - so a verb (freeze, step, screenshot, set, ...) always wins and an
 * app can never shadow one - and latches whatever nothing there claimed.
 * main.c's frame loop takes that latch and offers it to whichever app is
 * running.
 */
#pragma once

#include <stdbool.h>

#include "app.h"
#include "console/console_latch.h"
#include "console/console_verbs.h"

/* True if a verb claimed `line` (its reply, if any, already sent). False and
 * `line` latched into `unclaimed` otherwise, for the frame loop to take. */
static inline bool
console_dispatch_or_latch(console_registry_t* registry, const char* line, console_reply_fn reply,
                          console_latch_t* unclaimed) {
    if (console_registry_handle_line(registry, line, reply)) {
        return true;
    }
    console_latch_set(unclaimed, line);
    return false;
}

/* True if `app` is running, has a console_line callback, and that callback
 * claimed `line`. False for every other reason - no app, no callback, or
 * the callback declining it - so the caller can fall back to the same
 * "ignoring line" log a line no registered verb claimed already gets. */
static inline bool
console_app_line_offer(const app_t* app, const char* line) {
    return app != NULL && app->console_line != NULL && app->console_line(line);
}

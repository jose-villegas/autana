/*
 * console_tune: the console's lines for tunables (core/tune.h), parsed
 * against a registry, replies out through a callback. Pure, so a test drives
 * a registry it owns; the SET, GET, RESET and TUNE verbs
 * (console/verbs/console_tune.c) drive tune_shared().
 */
#pragma once

#include <stdbool.h>

#include "console/console_words.h"
#include "core/tune.h"

/* The verb registrations check their identifiers against these request words. */
#define CONSOLE_TUNE_SET   "SET"
#define CONSOLE_TUNE_GET   "GET"
#define CONSOLE_TUNE_RESET "RESET"
#define CONSOLE_TUNE_LIST  "TUNE"
#define CONSOLE_TUNE_OK    "TUNE_OK"
#define CONSOLE_TUNE_ERR   "TUNE_ERR"
#define CONSOLE_TUNE_END   "TUNE_END"

/* True if `line` was one of these, whatever came of it. Replies, one call
 * each:
 *   SET <name> <value>   TUNE_OK <name>=<value>, or TUNE_ERR <reason> ...
 *   GET <name>           the same
 *   RESET <name>         the same, the value being the initial one again
 *   TUNE                 TUNE <name>=<value> min=<low> max=<high> default=<initial>
 *                        per tunable, TUNE_ERR clash <name> per name declared
 *                        twice, then TUNE_END count=<n> */
bool console_tune_line(tune_registry_t* registry, const char* line, console_reply_fn reply);

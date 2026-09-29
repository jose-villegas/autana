/*
 * console_runsuite - RUNSUITE <name> [<pattern>[,<pattern>...]]: runs one
 * named self-test suite instead of the whole boot-time run, narrowed to the
 * tests whose name contains a pattern when any are given.
 * CONFIG_LAUNCHER_SELFTEST only - the verb itself only sets a latch;
 * main.c's frame loop is what actually calls suites_run_request(), for the
 * same reason SCREENSHOT does not draw from this task either (see console.c's
 * own top comment).
 */
#include "console/console_runsuite.h"
#include "console/console_latch.h"
#include "console/console_verbs.h"

#include "esp_log.h"
#include "suites.h"

static const char* TAG = "console";

static console_latch_t runsuite_latch;

static void
console_verb_runsuite(const char* args, console_reply_fn reply) {
    (void)reply;
    ESP_LOGI(TAG, "RUNSUITE %s", args);
    console_latch_set(&runsuite_latch, args);
}

/* The longest request suites_run_request() accepts: a name, a space, and
 * SUITE_FILTER_MAX patterns with a comma between each. The verb's own
 * static assert then holds CONSOLE_LINE_MAX to it. */
#define RUNSUITE_ARGS_MAX (SUITE_NAME_MAX + 1 + SUITE_FILTER_MAX * SUITE_FILTER_LEN)

CONSOLE_VERB(runsuite, RUNSUITE_ARGS_MAX, console_verb_runsuite)

bool
console_runsuite_take_request(char* name_out, size_t name_out_size) {
    return console_latch_take(&runsuite_latch, name_out, name_out_size);
}

/*
 * console_runsuite - RUNSUITE <name>: runs one named self-test suite
 * instead of the whole boot-time run (narrowed by TESTFILTER, below).
 * CONFIG_LAUNCHER_SELFTEST only - the
 * verb itself only sets a latch; main.c's frame loop is what actually
 * calls suites_run_one(), for the same reason SCREENSHOT does not draw
 * from this task either (see console.c's own top comment).
 */
#include "console/console_runsuite.h"
#include "console/console_latch.h"
#include "console/console_verbs.h"

#include <stdarg.h>

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

/* Headroom over the longest registered suite name today
 * (suite_control_center_layout, 27 chars) - bump this rather than trim a
 * name to fit it, the same reasoning suites.h's own SUITE_MAX comment
 * gives. */
CONSOLE_VERB(runsuite, 38, console_verb_runsuite)

/* The survey walk runs a suite's code between its tests without the tests, so
 * whatever it logs there (a summary, a perf line) would read as a verdict on
 * tests that never ran. */
static vprintf_like_t log_before_survey;

static int
discard_log(const char* format, va_list args) {
    (void)format;
    (void)args;
    return 0;
}

static void
silence_survey_logs(bool quiet) {
    if (quiet) {
        log_before_survey = esp_log_set_vprintf(discard_log);
    } else {
        esp_log_set_vprintf(log_before_survey);
    }
}

__attribute__((constructor)) static void
register_survey_hook(void) {
    suites_set_survey_hook(silence_survey_logs);
}

/* TESTFILTER <pattern> adds one substring the next RUNSUITE narrows its tests
 * to; bare TESTFILTER forgets them all. A line holds one pattern because the
 * console line is short. A refused pattern is logged for the host to read. */
static void
console_verb_testfilter(const char* args, console_reply_fn reply) {
    (void)reply;
    if (args[0] == '\0') {
        suites_filter_clear();
    } else if (!suites_filter_add(args)) {
        ESP_LOGE(TAG, "TESTFILTER refused '%s'", args);
    }
}

CONSOLE_VERB(testfilter, 37, console_verb_testfilter)

bool
console_runsuite_take_request(char* name_out, size_t name_out_size) {
    return console_latch_take(&runsuite_latch, name_out, name_out_size);
}

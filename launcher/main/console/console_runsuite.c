/*
 * console_runsuite - RUNSUITE <name>: runs one named self-test suite
 * instead of the whole boot-time run. CONFIG_LAUNCHER_SELFTEST only - the
 * verb itself only sets a latch; main.c's frame loop is what actually
 * calls suites_run_one(), for the same reason SCREENSHOT does not draw
 * from this task either (see console.c's own top comment).
 */
#include "console/console_runsuite.h"
#include "console/console_latch.h"
#include "console/console_verbs.h"

#include "esp_log.h"

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
CONSOLE_VERB(RUNSUITE, 38, console_verb_runsuite)

bool
console_runsuite_take_request(char* name_out, size_t name_out_size) {
    return console_latch_take(&runsuite_latch, name_out, name_out_size);
}

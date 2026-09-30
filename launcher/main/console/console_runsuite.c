/*
 * console_runsuite (RUNSUITE <name> [<pattern>[,<pattern>...]]): runs one
 * named self-test suite instead of the whole boot-time run, narrowed to the
 * tests whose name contains a pattern when any are given.
 * CONFIG_LAUNCHER_SELFTEST only: the verb itself only sets a latch;
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

/* The one copy of the request: the verb fills it, the frame loop reads it in
 * place. Sized by the limits, not by CONSOLE_LINE_MAX, so no other console
 * buffer grows for it. */
static char request[RUNSUITE_ARGS_MAX + 1];

static volatile enum { IDLE, PENDING, RUNNING } state;

static void
console_verb_runsuite(const char* args, console_reply_fn reply) {
    (void)reply;
    if (state != IDLE) {
        ESP_LOGW(TAG, "RUNSUITE refused: the previous one has not finished");
        return;
    }
    ESP_LOGI(TAG, "RUNSUITE %s", args);
    console_latch_copy(request, sizeof request, args);
    state = PENDING;
}

CONSOLE_VERB_LONG(runsuite, RUNSUITE_ARGS_MAX, RUNSUITE_LINE_MAX, console_verb_runsuite)

const char*
console_runsuite_take_request(void) {
    if (state != PENDING) {
        return NULL;
    }
    state = RUNNING;
    return request;
}

void
console_runsuite_finish(void) {
    state = IDLE;
}

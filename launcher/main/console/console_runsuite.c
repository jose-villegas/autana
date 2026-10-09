/*
 * console_runsuite (RUNSUITE <name> [<pattern>[,<pattern>...]]): runs one
 * self-test suite, narrowed to tests whose name contains a pattern.
 * CONFIG_LAUNCHER_SELFTEST only. The verb posts a frame request, as this
 * task may not draw (console.c); the shell frame loop calls
 * suites_run_request().
 */
#include "console/console_runsuite.h"
#include "console/console_frame_request.h"
#include "console/console_verbs.h"

#include "esp_log.h"

#include <stdio.h>

static const char* TAG = "console";

static void
console_verb_runsuite(const char* args, console_reply_fn reply) {
    (void)reply;
    console_frame_request_t request = {.kinds = CONSOLE_FRAME_RUNSUITE};
    (void)snprintf(request.suite, sizeof request.suite, "%s", args);
    if (!console_frame_post(console_frame_mailbox(), &request)) {
        ESP_LOGW(TAG, "RUNSUITE refused: the previous one has not finished");
        return;
    }
    ESP_LOGI(TAG, "RUNSUITE %s", args);
}

CONSOLE_VERB_LONG(runsuite, RUNSUITE_ARGS_MAX, RUNSUITE_LINE_MAX, console_verb_runsuite)

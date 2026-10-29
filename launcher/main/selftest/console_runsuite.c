/*
 * console_runsuite (RUNSUITE <name> [<pattern>[,<pattern>...]] | RUNSUITE ?):
 * runs one self-test suite, narrowed to tests whose name contains a pattern,
 * or lists the registered suites.
 * CONFIG_LAUNCHER_SELFTEST only. The verb posts a frame request, as this
 * task may not draw (console.c); the shell frame loop calls
 * suites_run_request().
 */
#include "selftest/console_runsuite.h"
#include "console/console_verbs.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char* TAG = "console";

#define RUNSUITE_LIST_QUERY "?"
#define SUITE_COUNT_DIGITS  3
_Static_assert(TESTKIT_SUITE_MAX < 1000, "SUITE_COUNT_DIGITS holds TESTKIT_SUITE_MAX");
#define RUNSUITE_LIST_BYTES (sizeof "RUNSUITE_SUITE name= on_request=0" + SUITE_NAME_MAX)

CONSOLE_FRAME_SLOT(console_runsuite_slot, console_runsuite_request_t, true, NULL)

typedef struct {
    console_reply_fn reply;
    int count;
} runsuite_listing_t;

static void
list_one(const char* name, bool on_request, void* context) {
    runsuite_listing_t* listing = context;
    char line[RUNSUITE_LIST_BYTES];
    (void)snprintf(line, sizeof line, "RUNSUITE_SUITE name=%s on_request=%d", name, on_request ? 1 : 0);
    listing->reply(line);
    listing->count++;
}

/* `runsuite ?` lists the suites this image registers, answered here rather
 * than on the frame loop: the registry never changes once main() starts. */
static void
list_suites(console_reply_fn reply) {
    runsuite_listing_t listing = {.reply = reply};
    suites_each(list_one, &listing);
    char line[sizeof "RUNSUITE_END count=" + SUITE_COUNT_DIGITS];
    (void)snprintf(line, sizeof line, "RUNSUITE_END count=%d", listing.count);
    reply(line);
}

static void
console_verb_runsuite(const char* args, console_reply_fn reply) {
    if (strcmp(args, RUNSUITE_LIST_QUERY) == 0) {
        list_suites(reply);
        return;
    }
    console_runsuite_request_t request;
    console_copy_text(request.suite, sizeof request.suite, args);
    if (!console_frame_post(console_frame_slots(), &console_runsuite_slot, &request)) {
        ESP_LOGW(TAG, "RUNSUITE refused: the previous one has not finished");
        return;
    }
    ESP_LOGI(TAG, "RUNSUITE %s", args);
}

CONSOLE_VERB_LONG(runsuite, RUNSUITE_ARGS_MAX, RUNSUITE_LINE_MAX, console_verb_runsuite)

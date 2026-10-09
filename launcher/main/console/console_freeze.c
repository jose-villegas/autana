/*
 * console_freeze: FREEZE, RESUME and STEP. See console_freeze.h.
 */
#include "console/console_freeze.h"
#include "console/console_verbs.h"

#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>

static const char* TAG = "freeze";

/* An upper bound on STEP's count, so a typo cannot hand the loop a run
 * long enough to look like RESUME. */
#define STEP_MAX      1000

/* The digits of STEP_MAX: what CONSOLE_VERB() sizes its line against. */
#define STEP_ARGS_MAX 4

/* All three verbs share one request kind: they are three ways of saying
 * what the loop should do next, so a later one replaces an earlier one
 * still waiting. */
static void
post(bool frozen, int steps) {
    const console_frame_request_t request = {.kinds = CONSOLE_FRAME_FREEZE, .frozen = frozen, .steps = steps};
    (void)console_frame_post(console_frame_mailbox(), &request);
}

static void
console_verb_freeze(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    post(true, 0);
}

CONSOLE_VERB(freeze, 0, console_verb_freeze)

static void
console_verb_resume(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    post(false, 0);
}

CONSOLE_VERB(resume, 0, console_verb_resume)

/* STEP with nothing after it, or less than 1, is STEP 1. */
static void
console_verb_step(const char* args, console_reply_fn reply) {
    (void)reply;
    long n = strtol(args, NULL, 10);
    if (n < 1) {
        n = 1;
    }
    post(true, n > STEP_MAX ? STEP_MAX : (int)n);
}

CONSOLE_VERB(step, STEP_ARGS_MAX, console_verb_step)

/* Read and written only on the frame loop. */
static bool frozen;
static int credit;

void
console_freeze_apply(const console_frame_request_t* requests) {
    if (!(requests->kinds & CONSOLE_FRAME_FREEZE)) {
        return;
    }
    frozen = requests->frozen;
    credit = requests->steps;
    /* On its own line for a harness, the same reason RUNSUITE_COMPLETE
     * prints one (shell/shell.c). */
    printf("\nFREEZE_STATE frozen=%d steps=%d\n", frozen ? 1 : 0, credit);
    fflush(stdout);
    ESP_LOGI(TAG, "frozen=%d steps=%d", frozen ? 1 : 0, credit);
}

bool
console_freeze_frame_allowed(void) {
    if (!frozen) {
        return true;
    }
    if (credit > 0) {
        credit--;
        return true;
    }
    return false;
}

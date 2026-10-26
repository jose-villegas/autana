/*
 * console_freeze: FREEZE, RESUME and STEP. See console_freeze.h.
 */
#include "console/verbs/console_freeze.h"
#include "console/console_frame_request.h"
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

typedef struct {
    bool frozen; /* FREEZE and STEP; RESUME clears it */
    int steps;   /* STEP's frame count */
} freeze_request_t;

/* Read and written only on the frame loop. */
static bool frozen;
static int credit;

static void
apply(const void* payload) {
    const freeze_request_t* request = payload;
    frozen = request->frozen;
    credit = request->steps;
    /* On its own line for a harness, the same reason RUNSUITE_COMPLETE
     * prints one (shell/shell.c). */
    printf("\nFREEZE_STATE frozen=%d steps=%d\n", frozen ? 1 : 0, credit);
    (void)fflush(stdout);
    ESP_LOGI(TAG, "frozen=%d steps=%d", frozen ? 1 : 0, credit);
}

/* All three verbs share one slot: they are three ways of saying what the
 * loop should do next, so a later one replaces an earlier one still
 * waiting. */
CONSOLE_FRAME_SLOT(console_freeze_slot, freeze_request_t, false, apply)

static void
post(bool hold, int steps) {
    const freeze_request_t request = {.frozen = hold, .steps = steps};
    (void)console_frame_post(console_frame_slots(), &console_freeze_slot, &request);
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

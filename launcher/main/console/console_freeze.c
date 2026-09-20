/*
 * console_freeze - FREEZE, RESUME and STEP. See console_freeze.h.
 */
#include "console/console_freeze.h"
#include "console/console_latch.h"
#include "console/console_verbs.h"

#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>

static const char* TAG = "freeze";

/* An upper bound on STEP's count, so a typo cannot hand the loop a run
 * long enough to look like RESUME. */
#define STEP_MAX      1000

/* The digits of STEP_MAX - what CONSOLE_VERB() sizes its line against. */
#define STEP_ARGS_MAX 4

/* One latch for all three verbs: they are three ways of saying the same
 * thing - what the loop should do next - so a later one replacing an
 * earlier one still waiting is right, which is exactly what a latch does
 * (console_latch.h). The leading character says which verb wrote it. */
static console_latch_t command;

static void
console_verb_freeze(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    console_latch_set(&command, "f");
}

CONSOLE_VERB(freeze, 0, console_verb_freeze)

static void
console_verb_resume(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    console_latch_set(&command, "r");
}

CONSOLE_VERB(resume, 0, console_verb_resume)

/* STEP with nothing after it is STEP 1. */
static void
console_verb_step(const char* args, console_reply_fn reply) {
    (void)reply;
    char line[STEP_ARGS_MAX + 2];
    line[0] = 's';
    console_latch_copy(line + 1, sizeof(line) - 1, args);
    console_latch_set(&command, line);
}

CONSOLE_VERB(step, STEP_ARGS_MAX, console_verb_step)

static int
step_credit(const char* digits) {
    if (digits[0] == '\0') {
        return 1;
    }
    const long n = strtol(digits, NULL, 10);
    if (n < 1) {
        return 1;
    }
    return (n > STEP_MAX) ? STEP_MAX : (int)n;
}

bool
console_freeze_frame_allowed(void) {
    /* Read and written only here, so the credit needs no atomicity of its
     * own: the console task's half of the handoff ends at the latch. */
    static bool frozen;
    static int credit;

    char args[CONSOLE_ARGS_MAX];
    if (console_latch_take(&command, args, sizeof args)) {
        switch (args[0]) {
            case 'f':
                frozen = true;
                credit = 0;
                break;
            case 'r':
                frozen = false;
                credit = 0;
                break;
            case 's':
                frozen = true;
                credit = step_credit(args + 1);
                break;
            default: break;
        }
        /* On its own line for a harness, the same reason
         * RUNSUITE_COMPLETE prints one (main.c). */
        printf("\nFREEZE_STATE frozen=%d steps=%d\n", frozen ? 1 : 0, credit);
        fflush(stdout);
        ESP_LOGI(TAG, "frozen=%d steps=%d", frozen ? 1 : 0, credit);
    }

    if (!frozen) {
        return true;
    }
    if (credit > 0) {
        credit--;
        return true;
    }
    return false;
}

/*
 * console_buildid - BUILDID, the console's way to ask what firmware is
 * running without waiting on a screenshot. Development builds only - see
 * console.h.
 */
#include "console/console_verbs.h"

#include "build_id_generated.h"

#include <stdio.h>

static void
console_verb_buildid(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    printf("BUILD_ID=%s\n", BUILD_ID);
    fflush(stdout);
}

CONSOLE_VERB(BUILDID, 0, console_verb_buildid)

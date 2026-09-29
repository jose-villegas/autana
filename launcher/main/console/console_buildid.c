/*
 * console_buildid - BUILDID, the console's way to ask what firmware is
 * running without waiting on a screenshot. Development builds only - see
 * console.h.
 */
#include "console/console_verbs.h"

#include "util/build_id.h"

#include <stdio.h>

static void
console_verb_buildid(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    printf("BUILD_ID=%s\n", build_id());
    fflush(stdout);
}

CONSOLE_VERB(buildid, 0, console_verb_buildid)

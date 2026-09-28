#include "console/console_frame_watch.h"
#include "console/console_latch.h"
#include "console/console_verbs.h"

#include "util/frame_watch.h"

#include <stdio.h>

static console_latch_t request;

static void
console_verb_framewatch(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    console_latch_set(&request, "");
}

CONSOLE_VERB(framewatch, 0, console_verb_framewatch)

void
console_frame_watch_answer(void) {
    char unused[1];
    if (!console_latch_take(&request, unused, sizeof unused)) {
        return;
    }
    char json[FRAME_WATCH_JSON_MAX];
    frame_watch_json(json, sizeof json);
    (void)printf("FRAMEWATCH %s\n", json);
    (void)fflush(stdout);
}

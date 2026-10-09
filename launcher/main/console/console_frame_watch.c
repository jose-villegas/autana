#include "console/console_frame_watch.h"
#include "console/console_frame_request.h"
#include "console/console_verbs.h"

#include "util/runtime/frame_watch.h"

#include <stdio.h>

static void
console_verb_framewatch(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    static const console_frame_request_t request = {.kinds = CONSOLE_FRAME_FRAMEWATCH};
    (void)console_frame_post(console_frame_mailbox(), &request);
}

CONSOLE_VERB(framewatch, 0, console_verb_framewatch)

void
console_frame_watch_answer(void) {
    char json[FRAME_WATCH_JSON_MAX];
    frame_watch_json(json, sizeof json);
    (void)printf("FRAMEWATCH %s\n", json);
    (void)fflush(stdout);
}

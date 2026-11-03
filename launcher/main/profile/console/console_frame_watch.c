/* console_frame_watch (FRAMEWATCH): the frame watch's counts and repeating
 * sites (profile/frame_watch.h) as one `FRAMEWATCH <json>` line. The verb only
 * posts; the frame loop answers, since the counts are its own. */
#include "console/console_frame_request.h"
#include "console/console_verbs.h"

#include "profile/frame_watch.h"

#include <stdio.h>

static void
answer(const void* payload) {
    (void)payload;
    char json[FRAME_WATCH_JSON_MAX];
    frame_watch_json(json, sizeof json);
    (void)printf("FRAMEWATCH %s\n", json);
    (void)fflush(stdout);
}

CONSOLE_FRAME_SIGNAL(console_frame_watch_slot, answer)

static void
console_verb_framewatch(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    (void)console_frame_post(console_frame_slots(), &console_frame_watch_slot, NULL);
}

CONSOLE_VERB(framewatch, 0, console_verb_framewatch)

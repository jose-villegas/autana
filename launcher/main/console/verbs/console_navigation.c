/* console_navigation: see console_navigation.h. */
#include "console/verbs/console_navigation.h"

CONSOLE_FRAME_SLOT(console_navigation_slot, console_navigation_request_t, false, NULL)

static void
post(console_navigation_t navigation, const char* app) {
    console_navigation_request_t request = {.navigation = navigation};
    console_copy_text(request.app, sizeof request.app, app);
    (void)console_frame_post(console_frame_slots(), &console_navigation_slot, &request);
}

static void
console_verb_apps(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    post(CONSOLE_NAVIGATION_APPS, "");
}

static void
console_verb_open(const char* args, console_reply_fn reply) {
    (void)reply;
    post(CONSOLE_NAVIGATION_OPEN, args);
}

static void
console_verb_home(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    post(CONSOLE_NAVIGATION_HOME, "");
}

CONSOLE_VERB(apps, 0, console_verb_apps)
CONSOLE_VERB(open, CONSOLE_LINE_MAX - 6, console_verb_open)
CONSOLE_VERB(home, 0, console_verb_home)

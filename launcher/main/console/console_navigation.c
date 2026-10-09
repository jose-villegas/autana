/* console_navigation: APPS, OPEN and HOME post a navigation the shell
 * applies at a frame boundary (shell.c). */
#include "console/console_frame_request.h"
#include "console/console_verbs.h"

#include <stdio.h>

static void
post(console_navigation_t navigation, const char* app) {
    console_frame_request_t request = {.kinds = CONSOLE_FRAME_NAVIGATE, .navigation = navigation};
    (void)snprintf(request.app, sizeof request.app, "%s", app);
    (void)console_frame_post(console_frame_mailbox(), &request);
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

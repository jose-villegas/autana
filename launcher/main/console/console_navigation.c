/* console_navigation - APPS, OPEN and HOME request shell navigation. */
#include "console/console_navigation.h"
#include "console/console_latch.h"
#include "console/console_verbs.h"

static console_latch_t request;
static console_navigation_t requested;

static void
set_request(console_navigation_t navigation, const char* args) {
    requested = navigation;
    console_latch_set(&request, args);
}

static void
console_verb_apps(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    set_request(CONSOLE_NAVIGATION_APPS, "");
}

static void
console_verb_open(const char* args, console_reply_fn reply) {
    (void)reply;
    set_request(CONSOLE_NAVIGATION_OPEN, args);
}

static void
console_verb_home(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    set_request(CONSOLE_NAVIGATION_HOME, "");
}

CONSOLE_VERB(apps, 0, console_verb_apps)
CONSOLE_VERB(open, CONSOLE_LINE_MAX - 6, console_verb_open)
CONSOLE_VERB(home, 0, console_verb_home)

bool
console_navigation_take_request(console_navigation_t* navigation, char* name, size_t name_size) {
    if (!console_latch_take(&request, name, name_size)) {
        return false;
    }
    *navigation = requested;
    return true;
}

/* console_navigation - APPS, OPEN and HOME request shell navigation. */
#include "console/console_navigation.h"
#include "console/console_latch.h"
#include "console/console_verbs.h"

static console_latch_t request;

static void
set_request(char command, const char* args) {
    char line[CONSOLE_ARGS_MAX];
    line[0] = command;
    console_latch_copy(line + 1, sizeof(line) - 1, args);
    console_latch_set(&request, line);
}

static void
console_verb_apps(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    set_request('a', "");
}

static void
console_verb_open(const char* args, console_reply_fn reply) {
    (void)reply;
    set_request('o', args);
}

static void
console_verb_home(const char* args, console_reply_fn reply) {
    (void)args;
    (void)reply;
    set_request('h', "");
}

CONSOLE_VERB(apps, 0, console_verb_apps)
CONSOLE_VERB(open, CONSOLE_LINE_MAX - 6, console_verb_open)
CONSOLE_VERB(home, 0, console_verb_home)

bool
console_navigation_take_request(console_navigation_t* navigation, char* name, size_t name_size) {
    char line[CONSOLE_ARGS_MAX];
    if (!console_latch_take(&request, line, sizeof line)) {
        return false;
    }
    switch (line[0]) {
        case 'a': *navigation = CONSOLE_NAVIGATION_APPS; break;
        case 'o': *navigation = CONSOLE_NAVIGATION_OPEN; break;
        default: *navigation = CONSOLE_NAVIGATION_HOME; break;
    }
    console_latch_copy(name, name_size, line + 1);
    return true;
}

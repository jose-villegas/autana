/*
 * console_perf: PERF arms the hardware counters on one frame_cost name and
 * lists the names and events; the counts arrive with frame_cost's own report.
 * Development builds only; see console.h.
 */
#include "console/console.h"
#include "console/console_verbs.h"

#include "util/frame_cost.h"

#include <stdio.h>
#include <string.h>

static void
list_names_and_events(void) {
    for (int i = 0; frame_cost_name_at(i) != NULL; i++) {
        printf("PERFMON_NAME %s\n", frame_cost_name_at(i));
    }
    if (frame_cost_names_dropped() > 0) {
        printf("PERFMON_NAMES_DROPPED %d\n", frame_cost_names_dropped());
    }
    for (int i = 0; i < frame_cost_event_count(); i++) {
        printf("PERFMON_EVENT %s\n", frame_cost_event_at(i)->name);
    }
    printf("PERFMON_END\n");
}

static void
console_verb_perf(const char* args, console_reply_fn reply) {
    char line[CONSOLE_LINE_MAX + 32];
    char name[FRAME_COST_NAME_MAX + 1] = "";
    char event[FRAME_COST_EVENT_NAME_MAX + 1] = FRAME_COST_DEFAULT_EVENT;
    char extra[2];
    const int fields = sscanf(args, "%24s %17s %1s", name, event, extra);

    if (fields <= 0 || strcmp(name, "?") == 0) {
        list_names_and_events();
        return;
    }
    if (fields > 2) {
        snprintf(line, sizeof line, "PERFMON_ERR usage: PERF <name|off|?> [event]");
    } else if (strcmp(name, "off") == 0) {
        snprintf(line, sizeof line, "%s",
                 frame_cost_request_arm("", FRAME_COST_DEFAULT_EVENT) ? "PERFMON_OK off" : "PERFMON_ERR busy");
    } else if (!frame_cost_name_known(name)) {
        snprintf(line, sizeof line, "PERFMON_ERR unknown name %s (PERF lists the names seen)", name);
    } else if (frame_cost_event_find(event) == NULL) {
        snprintf(line, sizeof line, "PERFMON_ERR unknown event %s", event);
    } else if (!frame_cost_request_arm(name, event)) {
        snprintf(line, sizeof line, "PERFMON_ERR busy");
    } else {
        snprintf(line, sizeof line, "PERFMON_OK %s %s", name, event);
    }
    reply(line);
}

CONSOLE_VERB(PERF, FRAME_COST_NAME_MAX + 1 + FRAME_COST_EVENT_NAME_MAX, console_verb_perf)

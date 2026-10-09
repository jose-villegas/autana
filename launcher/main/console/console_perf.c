/*
 * console_perf: PERF arms the hardware counters on one frame_cost name and
 * lists the names and events; the counts arrive on the shell's own `perf:`
 * line. Development builds only; see console.h.
 */
#include "console/console.h"
#include "console/console_perf_parse.h"
#include "console/console_verbs.h"

#include "core/frame_cost.h"

static const char*
event_at(int index) {
    const frame_cost_event_t* const event = frame_cost_event_at(index);
    return event != NULL ? event->name : NULL;
}

static void
console_verb_perf(const char* args, console_reply_fn reply) {
    static const console_perf_ops_t ops = {
        .name_index = frame_cost_shared_name_index,
        .event_index = frame_cost_event_index,
        .name_at = frame_cost_name_at,
        .event_at = event_at,
        .names_dropped = frame_cost_names_dropped,
        .post_arm = frame_cost_shared_post_arm,
    };
    console_perf_dispatch(args, &ops, reply);
}

CONSOLE_VERB(PERF, FRAME_COST_NAME_MAX + 1 + FRAME_COST_EVENT_NAME_MAX, console_verb_perf)

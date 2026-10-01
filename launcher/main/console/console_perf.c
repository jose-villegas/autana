/*
 * console_perf: PERF owns the command line and delegates the selected region
 * and counter event to util/perf_region.
 */
#include "console/console.h"
#include "console/console_verbs.h"

#include "util/perf_region.h"

static void
console_verb_perf(const char* args, console_reply_fn reply) {
    (void)reply;
    perf_region_handle_line(args, console_reply_stdio);
}

CONSOLE_VERB(perf, PERF_REGION_NAME_MAX + 1 + PERF_REGION_EVENT_NAME_MAX, console_verb_perf)

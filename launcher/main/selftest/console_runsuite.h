/* console_runsuite: the RUNSUITE line's limits and its slot, which the
 * frame loop alone takes and answers, since only it may run a suite. */
#pragma once

#include "console/console_frame_request.h"
#include "suites.h"

/* The longest request suites_run_request() accepts: a name, a space, and
 * SUITE_FILTER_MAX patterns with a comma between each. */
#define RUNSUITE_ARGS_MAX (SUITE_NAME_MAX + 1 + (SUITE_FILTER_MAX * SUITE_FILTER_LEN))

/* The console line that carries it, and the size of the reader's buffer. */
#define RUNSUITE_LINE_MAX (sizeof("runsuite") + 1 + RUNSUITE_ARGS_MAX)

typedef struct {
    char suite[RUNSUITE_ARGS_MAX + 1]; /* a suite name, then any patterns */
} console_runsuite_request_t;

extern console_frame_slot_t console_runsuite_slot;

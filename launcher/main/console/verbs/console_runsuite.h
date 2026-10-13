/* console_runsuite: the RUNSUITE line's limits; the request itself rides
 * console_frame_request.h to the frame loop, which alone may run a suite. */
#pragma once

#include "suites.h"

/* The longest request suites_run_request() accepts: a name, a space, and
 * SUITE_FILTER_MAX patterns with a comma between each. */
#define RUNSUITE_ARGS_MAX (SUITE_NAME_MAX + 1 + SUITE_FILTER_MAX * SUITE_FILTER_LEN)

/* The console line that carries it, and the size of the reader's buffer. */
#define RUNSUITE_LINE_MAX (sizeof("runsuite") + 1 + RUNSUITE_ARGS_MAX)

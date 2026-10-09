/* console_runsuite: suite requests retained for execution by the frame loop. */
#pragma once

#include <stdbool.h>

#include "suites.h"

/* The longest request suites_run_request() accepts: a name, a space, and
 * SUITE_FILTER_MAX patterns with a comma between each. */
#define RUNSUITE_ARGS_MAX (SUITE_NAME_MAX + 1 + SUITE_FILTER_MAX * SUITE_FILTER_LEN)

/* The console line that carries it, and the size of the reader's buffer. */
#define RUNSUITE_LINE_MAX (sizeof("runsuite") + 1 + RUNSUITE_ARGS_MAX)

/* A RUNSUITE line for the frame loop, which alone may run a suite: a suite
 * draws, clears and presents, and must never interleave with the shell's own
 * frame loop on another task (see console.c's own top comment).
 *
 * Returns the pending request (a suite name, then any patterns) or NULL.
 * The pointer stays valid until console_runsuite_finish(), and a RUNSUITE
 * that arrives before then is refused rather than overwriting it. */
const char* console_runsuite_take_request(void);
void console_runsuite_finish(void);

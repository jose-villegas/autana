#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Same "read and consume once per frame" contract as
 * console_screenshot_take_request() (console_screenshot.h), for a RUNSUITE
 * line - see console.c's own top comment for why a suite run needs this
 * even more: suites_run_one() draws, clears, and presents repeatedly, and
 * must never interleave with the shell's own frame loop on a different
 * task. Copies the pending suite name into `name_out` (caller-owned,
 * NUL-terminated) and returns true if a RUNSUITE line arrived; false
 * otherwise. */
bool console_runsuite_take_request(char* name_out, size_t name_out_size);

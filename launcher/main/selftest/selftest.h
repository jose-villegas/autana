/*
 * selftest: the board checking itself. The power-on self test (post.h)
 * runs at every boot of every build; the suite runner below exists only in
 * a CONFIG_LAUNCHER_SELFTEST build. An app may run either again.
 */
#pragma once

#include <stdbool.h>

/* Runs every test suite on the device and reports to the console.
 * Returns the number of failures; zero means everything passed.
 *
 * Called at boot, after the display is up (the graphics suite needs a live
 * framebuffer) and before the launcher takes over, and again by any app that
 * offers a rerun. */
int selftest_run(void);

/* Tests run inside an app visit: a run that starts with no app running (at
 * boot, or a RUNSUITE from the launcher) makes one for itself. Returns
 * whether it did, for selftest_visit_end(). */
bool selftest_visit_begin(void);
void selftest_visit_end(bool owned);

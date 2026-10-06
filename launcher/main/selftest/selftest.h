/*
 * selftest: the board checking itself. The power-on self test (post.h)
 * runs at every boot of every build; the suite runner below exists only in
 * a CONFIG_LAUNCHER_SELFTEST build. An app may run either again.
 */
#pragma once

/* Runs every test suite on the device and reports to the console.
 * Returns the number of failures; zero means everything passed.
 *
 * Called at boot, after the display is up (the graphics suite needs a live
 * framebuffer) and before the launcher takes over, and again by any app that
 * offers a rerun. */
int selftest_run(void);

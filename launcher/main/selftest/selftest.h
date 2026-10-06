/*
 * selftest: the board checking itself, apart from the boot that starts it.
 * Two checks live here: the power-on self test (post.h), which every build
 * runs on its hardware, and the on-board suite runner below, which only a
 * CONFIG_LAUNCHER_SELFTEST build carries.
 */
#pragma once

/* Runs every test suite on the device and reports to the console.
 * Returns the number of failures; zero means everything passed.
 *
 * Called at boot, after the display is up (the graphics suite needs a live
 * framebuffer) and before the launcher takes over. */
int selftest_run(void);

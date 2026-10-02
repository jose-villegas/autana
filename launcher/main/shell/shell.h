/*
 * shell: the runtime boot hands the main task to. It owns the one frame loop
 * and the switching between the launcher and whichever app is running; apps
 * only ever see app.h's callbacks.
 */
#pragma once

/* The shell's own state, settled first thing at boot: the self-tests step
 * the launcher and enter apps before the loop ever runs. */
void shell_init(void);

/* Development builds stop here, loudly, on two console prefixes that clash.
 * Called after the self-tests, so a clash never hides their report. */
void shell_check_console_prefixes(void);

/* The home screen as its first frame will draw it, untouched and whole; the
 * boot animation dissolves into it. */
void shell_paint_home_under_boot(void);

/* The frame loop. It never returns: once firmware goes idle on this board the
 * chip stops responding to reset signalling and can only be recovered with
 * the BOOT button; see docs/notes/Flashing-and-Toolchain.md. */
void shell_run(void) __attribute__((noreturn));

/*
 * shell: the runtime boot hands the main task to. It owns the one frame loop
 * and the switching between the launcher and whichever app is running; apps
 * only ever see app.h's callbacks.
 */
#pragma once

/* What the loop needs settled before the launcher is built. Development
 * builds also stop here, loudly, on two console prefixes that clash. */
void shell_init(void);

/* The home screen as its first frame will draw it, untouched and whole; the
 * boot animation dissolves into it. */
void shell_paint_home_under_boot(void);

/* The frame loop. It never returns: once firmware goes idle on this board the
 * chip stops responding to reset signalling and can only be recovered with
 * the BOOT button; see docs/notes/Flashing-and-Toolchain.md. */
void shell_run(void) __attribute__((noreturn));

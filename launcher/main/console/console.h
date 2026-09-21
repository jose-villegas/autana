/*
 * console - the device's one blocking reader on the board's serial console,
 * feeding whatever line it assembles to console_shared()'s registry.
 * Development builds only: a release image has nobody watching the serial
 * console to type a verb into, the same reasoning an app's own
 * developer-only instrumentation is gated on (see
 * docs/Build-Variants.md's "Development-only instrumentation" section).
 *
 * USB-Serial/JTAG, not UART: this board's USB-C is the S3's own peripheral
 * and the console's primary channel, so this listener sees the bytes
 * idf_monitor does. A CONFIG_LAUNCHER_QEMU image has its console on UART0
 * instead, the one port the emulator exposes, and the same calls are made
 * on that driver.
 *
 * Its own task, because console_start() switches the fd to the driver's
 * interrupt-driven reader, which is what lets a read block instead of the
 * frame loop polling every frame. There is room for exactly one blocking
 * reader on this stream - a second task reading it would race this one for
 * every byte, which is why every console verb answers from here, whichever
 * file's CONSOLE_VERB() owns it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "console/console_verbs.h"

/* Starts the background task that listens on the console for a verb line.
 * Call once, from app_main() - the same place and pattern as
 * touch_start()/buttons_start(): a small dedicated task the shell never
 * talks to directly, a verb's own result read back out through its own
 * accessor (console_screenshot_take_request(), console_runsuite_take_request()). */
void console_start(void);

/* Sends `prefix` then `payload` then a newline, each through the console
 * driver's own write rather than stdio - see console.c's own comment on why
 * a multi-hundred-KB capture cannot go through buffered stdio. For a verb
 * whose reply is too large or too timing-sensitive for a single printf(),
 * such as SCREENSHOT's framebuffer stream (console_screenshot.c). */
void console_emit_line(const char* prefix, const char* payload);

/* printf()+fflush() reply: used by every SET/GET/RESET/TUNE reply
 * (console_tune.c) and by this file's own fallback dispatch. A tune
 * reply is one short line, so buffered stdio is fine - unlike SCREENSHOT's
 * own console_emit_line() above. */
void console_reply_stdio(const char* line);

/* True once per unclaimed line, `out` filled; false with `out` untouched. */
bool console_take_unclaimed_line(char* out, size_t out_size);

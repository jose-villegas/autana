#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app.h"

/* Reads and clears whether the SCREENSHOT verb has been seen since the
 * last call - the same "read and consume once per frame" contract
 * buttons_read() already uses (see input/buttons.h), and for the same
 * reason: main.c's loop is the only place that should act on a request,
 * and only once per request, however many frames it takes main.c to get
 * back around to checking. */
bool console_screenshot_take_request(void);

/* Streams the framebuffer to stdout as base64 BMP, then one
 * SCREENSHOT_STATE: line of JSON device state from the same frame -
 * framed between SCREENSHOT_BEGIN/END lines a host script greps for.
 * `input` is passed in rather than read fresh, so touch/button fields
 * describe the exact frame the image does, not what the listener saw
 * later. `current_app` lets its OPTIONAL diagnostic_json splice in an
 * "app" key. Call after a frame is drawn, before presenting, so capture
 * matches what's about to appear. */
void console_screenshot_dump(const input_t* input, const app_t* current_app);

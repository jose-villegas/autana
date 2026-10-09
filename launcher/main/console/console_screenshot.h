/* console_screenshot: the framebuffer dump with device state, for a SCREENSHOT the frame loop took. */
#pragma once

#include "app/app.h"

/* Streams the framebuffer to stdout as base64 BMP, then one
 * SCREENSHOT_STATE: line of JSON device state from the same frame,
 * framed between SCREENSHOT_BEGIN/END lines a host script greps for.
 * `input` is passed in rather than read fresh, so touch/button fields
 * describe the exact frame the image does, not what the listener saw
 * later. `current_app` lets its OPTIONAL diagnostic_json splice in an
 * "app" key. Call after a frame is drawn, before presenting, so capture
 * matches what's about to appear. */
void console_screenshot_dump(const input_t* input, const app_t* current_app);

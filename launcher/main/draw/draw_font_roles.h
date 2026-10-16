/*
 * draw_font_roles: which typeface plays which part, decided once, here, so
 * retyping the UI is an edit here rather than a grep.
 *
 * The UI and boot title use the same bitmap font. Scale is a call-site
 * argument, so labels do not need a separate role.
 */
#pragma once

#include "draw/draw_font.h"

/* The UI/body-text role. Compile-time resolution lets the linker omit a
 * font nothing names. */
static inline const draw_font_t*
draw_font_ui(void) {
    return &draw_font_8x8;
}

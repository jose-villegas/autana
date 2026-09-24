/*
 * gfx_font_roles - which typeface plays which part, decided once, here, so
 * retyping the UI is an edit here rather than a grep.
 *
 * The UI and boot title use the same bitmap font. Scale is a call-site
 * argument, so labels do not need a separate role.
 */
#pragma once

#include "gfx/gfx_font.h"

/* The UI/body-text role. Compile-time resolution lets the linker omit a
 * font nothing names. */
static inline const gfx_font_t*
gfx_font_ui(void) {
    return &gfx_font_8x8;
}

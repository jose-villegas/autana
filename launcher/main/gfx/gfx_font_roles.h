/*
 * gfx_font_roles - which typeface plays which part, decided once, here, so
 * retyping the UI is an edit here rather than a grep.
 *
 * The UI and boot title use the same bitmap font. Scale is a call-site
 * argument, so labels do not need a separate role.
 */
#pragma once

#include "gfx/gfx_font.h"

/* The UI/body-text role - see this file's top comment for what draws with
 * it and why no second role exists yet. `static inline` rather than an
 * exported symbol, so a call to gfx_font_ui() compiles down to exactly what
 * naming `&gfx_font_8x8` by hand would have - the role's NAME appears in the
 * source, but nothing about how the reference resolves at compile time
 * changes from naming the font directly. */
static inline const gfx_font_t*
gfx_font_ui(void) {
    return &gfx_font_8x8;
}

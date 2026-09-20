/*
 * render_lab - what every scene in this app shares with the app itself: the
 * background colour, the partial-update toggle, the band clear, and the
 * last-frame coverage a scene marks dirty. app_render_lab.c defines all of
 * it; a scene includes this rather than keeping a copy.
 */
#pragma once

#include <stdbool.h>

#include "gfx/gfx.h"

#define RENDER_LAB_BACKGROUND_RGB 0x0A0C14

/* Whether a full-framebuffer scene erases only last frame's coverage instead
 * of the whole screen. A developer toggle, so it survives leaving the app. */
extern bool render_lab_partial_updates;

/* A band has no retained frame to erase a box out of, so the app fills each
 * touched band before the scene draws into it. */
void render_lab_clear_band(gfx_color_t* buf, int height);

typedef struct {
    int x0, y0, x1, y1; /* half-open, already clipped to the screen */
    bool valid;
} render_lab_coverage_t;

/* Marks last frame's box and this frame's dirty, then remembers this one: a
 * region the scene left still needs erasing though nothing overlaps it now.
 * `have` false means the scene drew nothing this frame. */
void render_lab_coverage_mark(render_lab_coverage_t* last, bool have, int x0, int y0, int x1, int y1);

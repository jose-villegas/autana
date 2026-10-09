/*
 * render_lab: the software-rendering testbed app. This header is what every
 * scene shares with the app itself: the background colour, the partial-
 * update toggle and the band clear. app_render_lab.c defines all of it; a scene includes this rather
 * than keeping a copy.
 */
#pragma once

#include <stdbool.h>

#include "gfx/draw/gfx_color.h"

#define RENDER_LAB_BACKGROUND_RGB 0x0A0C14

/* Whether a full-framebuffer scene erases only last frame's coverage instead
 * of the whole screen. A developer toggle, so it survives leaving the app. */
extern bool render_lab_partial_updates;

/* Optional camera selected when a loaded scene is entered; NULL chooses its first. */
extern const char* render_lab_start_camera;

/* A band has no retained frame to erase a box out of, so the app fills each
 * touched band before the scene draws into it. */
void render_lab_clear_band(gfx_color_t* buf, int height);

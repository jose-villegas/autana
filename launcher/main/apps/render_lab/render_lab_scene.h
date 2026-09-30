/*
 * render_lab_scene: what a scene provides so app_render_lab.c can host any
 * number of them behind one HUD, one BOOT menu and one scene picker.
 *
 * The app owns gfx_mode_enter()/exit(), the layout switch, BOOT handling,
 * the menu, the fps counter, draw_fps(), and the orientation-generation
 * check. A scene owns only its own
 * geometry, pose, clear colour and coverage/dirty marking.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/gfx.h"

typedef struct {
    const char* name;    /* shown on the HUD and the menu's scene picker */
    const char* key;     /* short, stable, lowercase; a start request names one of these */
    void (*enter)(void); /* allocate, reset pose; layout is already entered */

    /* Outside band mode, advances by dt_ms and draws. In band mode
     * (band_mode_active) it only advances and bins, for frame_band() below
     * to draw per touched band right after. */
    void (*frame)(uint32_t dt_ms, bool band_mode_active);

    void (*frame_band)(gfx_color_t* buf, int row0, int row1); /* band mode: draws [row0, row1) into buf */
    void (*exit)(void);                                       /* free what enter() allocated */
    void (*invalidate)(void);                                 /* forget last-frame coverage */

    /* A short status string shown after the scene name on the HUD line -
     * vertex/edge counts, say. NULL (the cube's default) shows nothing extra. */
    const char* (*status)(void);

    /* True for a scene with a retained, progressively-filled picture: the
     * app grants GFX_LAYOUT_FULL_FB regardless of render_lab_band_mode, and
     * never calls frame_band(), which such a scene may leave NULL. */
    bool needs_full_framebuffer;

    /* True for a scene that draws a lit mesh and gives its frame to
     * raster_show() with render_lab_view(), when tunables are enabled. */
    bool shows_view_modes;

    /* Optional. Runs while the previous frame is still being sent to the
     * panel, so it may only touch the scene's own memory; never gfx or the
     * framebuffer (app.h's update() contract). Not called while the menu is
     * open. */
    void (*update)(uint32_t dt_ms);
} render_lab_scene_t;

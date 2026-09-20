/*
 * render_lab_hud_screen - the fps readout in the top-left corner, a scene's
 * status under it, and the scene's name while it is newly entered.
 *
 * app_render_lab.c owns the fps clock, the title's lifetime and the
 * perf-test x override this reads; this file only turns them into
 * mu_text()/mu_draw_rect() calls, the split docs/Building-a-Screen.md asks
 * every screen to keep.
 */
#pragma once

#include <stdint.h>

#include "microui.h"

typedef struct {
    double fps_value;
    const char* scene_title;
    uint8_t scene_title_alpha; /* dithered coverage of the title's ink; 0 draws no title at all */

    /* A second line under the fps reading - a scene's vertex/edge counts,
     * say. NULL for none. */
    const char* status;

    /* -1 leaves the box at the corner inset - see draw_overlay_box()'s own comment
     * (render_lab_hud_screen.c) for why a perf test wants to move it
     * instead. */
    int fps_box_x_override;
} render_lab_hud_screen_state_t;

/* Draws the HUD. Caller brackets this with
 * ui_begin()/ui_end*() - see app_render_lab.c's draw_fps(), which picks
 * ui_end() vs ui_end_for_bands() by render path, not this file's concern. */
void render_lab_hud_screen_draw(mu_Context* ctx, const render_lab_hud_screen_state_t* state);

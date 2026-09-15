/*
 * cube_hud_screen - the persistent fps readout's microui drawing.
 *
 * app_cube.c owns the fps clock and the perf-test x override this reads;
 * this file only turns them into mu_text()/mu_draw_rect() calls, the split
 * docs/Building-a-Screen.md asks every screen to keep.
 */
#pragma once

#include "microui.h"

typedef struct {
    double fps_value;

    /* -1 centres the box - see draw_overlay_box()'s own comment
     * (cube_hud_screen.c) for why a perf test wants to move it instead. */
    int fps_box_x_override;
} cube_hud_screen_state_t;

/* Draws the fps line. Caller brackets this with ui_begin()/ui_end*() - see
 * app_cube.c's draw_fps(), which picks ui_end() vs ui_end_for_bands() by
 * render path, not this file's concern. */
void cube_hud_screen_draw(mu_Context* ctx, const cube_hud_screen_state_t* state);

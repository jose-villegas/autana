/*
 * render_lab_hud_screen: microui drawing for the fps readout, scene status,
 * and a scene title.
 *
 * The caller owns the timing and supplies every value this screen draws.
 */
#pragma once

#include <stdint.h>

#include "microui.h"

typedef struct {
    float fps_value;
    const char* scene_title;
    uint8_t scene_title_alpha; /* dithered coverage of the title's ink; 0 draws no title at all */

    /* A second line under the fps reading - a scene's mesh details,
     * say. NULL for none. */
    const char* status;

} render_lab_hud_screen_state_t;

/* Caller brackets drawing with the appropriate ui_begin()/ui_end() pair. */
void render_lab_hud_screen_draw(mu_Context* ctx, const render_lab_hud_screen_state_t* state);

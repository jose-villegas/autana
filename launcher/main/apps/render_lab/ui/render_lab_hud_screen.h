/*
 * render_lab_hud_screen: the face drawing for a scene title and, in a
 * development or host build, the fps readout and scene status.
 *
 * The caller owns the timing and supplies every value this screen draws.
 */
#pragma once

#include <stdint.h>

#include "build/build_variant.h"

typedef struct {
    const char* scene_title;
    uint8_t scene_title_alpha; /* dithered coverage of the title's ink; 0 draws no title at all */

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    float fps_value;
    /* A second line under the fps reading - a scene's mesh details,
     * say. NULL for none. */
    const char* status;
#endif
} render_lab_hud_screen_state_t;

/* Caller brackets drawing with the appropriate ui_begin()/ui_end() pair. */
void render_lab_hud_screen_draw(const render_lab_hud_screen_state_t* state);

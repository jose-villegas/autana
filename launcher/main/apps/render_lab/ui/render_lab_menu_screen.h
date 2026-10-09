/* render_lab_menu_screen: the BOOT-opened scene picker. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "microui.h"

typedef struct {
    const char* scene_name; /* shown on the cycle-scene button */
} render_lab_menu_screen_state_t;

typedef struct {
    bool next_scene_clicked;
} render_lab_menu_screen_result_t;

/* Draws the scene picker and the closing hint. Caller
 * brackets this with ui_begin()/ui_end*(). `dt_ms` drives the scroll
 * view's own momentum, unused while this screen keeps the default (none). */
render_lab_menu_screen_result_t
render_lab_menu_screen_draw(mu_Context* ctx, const render_lab_menu_screen_state_t* state, uint32_t dt_ms);

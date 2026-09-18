/*
 * cube_menu_screen - the BOOT-opened runtime-options menu's microui drawing.
 *
 * app_cube.c owns the two toggles this reads and what flipping either one
 * actually does (gfx_invalidate(), cube_mode_switch_request()); this file
 * only turns their current value into buttons and reports which one, if
 * any, this frame's tap landed on - the split docs/Building-a-Screen.md
 * asks every screen to keep.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "microui.h"

typedef struct {
    bool partial_updates_on;
    bool band_mode_on;
} cube_menu_screen_state_t;

typedef struct {
    bool partial_updates_clicked;
    bool band_mode_clicked;
} cube_menu_screen_result_t;

/* Draws both toggle rows and the closing hint. Caller brackets this with
 * ui_begin()/ui_end*(). `dt_ms` drives the scroll view's own momentum,
 * unused while this screen keeps the default (none). */
cube_menu_screen_result_t cube_menu_screen_draw(mu_Context* ctx, const cube_menu_screen_state_t* state, uint32_t dt_ms);

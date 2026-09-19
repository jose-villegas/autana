/*
 * render_lab_menu_screen - the BOOT-opened runtime-options menu's microui
 * drawing.
 *
 * app_render_lab.c owns the toggles and the scene picker this reads, and
 * what flipping or clicking one actually does; this file only turns their
 * current value into buttons and reports which one, if any, this frame's
 * tap landed on - the split docs/Building-a-Screen.md asks every screen to
 * keep.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "microui.h"

typedef struct {
    bool partial_updates_on;
    bool band_mode_on;
    const char* scene_name; /* shown on the cycle-scene button */
} render_lab_menu_screen_state_t;

typedef struct {
    bool partial_updates_clicked;
    bool band_mode_clicked;
    bool next_scene_clicked;
} render_lab_menu_screen_result_t;

/* Draws the toggle rows, the scene picker and the closing hint. Caller
 * brackets this with ui_begin()/ui_end*(). `dt_ms` drives the scroll
 * view's own momentum, unused while this screen keeps the default (none). */
render_lab_menu_screen_result_t
render_lab_menu_screen_draw(mu_Context* ctx, const render_lab_menu_screen_state_t* state, uint32_t dt_ms);

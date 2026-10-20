/* render_lab_menu_screen: the BOOT-opened scene picker. */
#pragma once

#include <stdbool.h>

/* Draws a scene's rows into the menu's flow, each `width` wide and centred:
 * ui_row_centre(width, h), then a widget at ui_next(). */
typedef void (*render_lab_controls_fn)(int width);

typedef struct {
    render_lab_controls_fn controls;
    const char* scene_name; /* shown on the cycle-scene button */
} render_lab_menu_screen_state_t;

typedef struct {
    bool next_scene_clicked;
} render_lab_menu_screen_result_t;

/* Draws the scene picker and the closing hint. Caller
 * brackets this with ui_begin()/ui_end*(). */
render_lab_menu_screen_result_t render_lab_menu_screen_draw(const render_lab_menu_screen_state_t* state);

/* render_lab_menu_screen: the BOOT-opened scene menu. */
#pragma once

#include <stdbool.h>

/* The SMALL FILL row's caption, and its dropdown's identity. */
#define RENDER_LAB_SMALL_FILL_LABEL "SMALL FILL:"
#define RENDER_LAB_UPSCALE_LABEL    "UPSCALE:"

/* Draws a scene's rows into the menu's flow, each `width` wide and centred:
 * ui_row_centre(width, h), then a widget at ui_next(). */
typedef void (*render_lab_controls_fn)(int width);

typedef struct {
    render_lab_controls_fn controls;
    const char* const* families; /* the Scene dropdown's entries; none = no Scene row */
    int family_count;
    int family;                  /* index of the showing one */
    const char* const* variants; /* the showing family's scenes, drawn when it has more than one */
    int variant_count;
    int variant; /* index of the showing one */
} render_lab_menu_screen_state_t;

typedef struct {
    int family_picked;  /* a family the user chose this frame, else -1 */
    int variant_picked; /* a variant likewise */
} render_lab_menu_screen_result_t;

/* A caption row, `width` wide and centred, above a widget. */
void render_lab_menu_caption(int width, const char* text);

/* Draws the scene picker and the closing hint. Caller
 * brackets this with ui_begin()/ui_end*(). */
render_lab_menu_screen_result_t render_lab_menu_screen_draw(const render_lab_menu_screen_state_t* state);

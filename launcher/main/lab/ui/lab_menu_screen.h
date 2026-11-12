/* lab_menu_screen: the BOOT-opened scene menu. */
#pragma once

#include <stdbool.h>

/* Draws a scene's rows into the menu's flow, each `width` wide and centred:
 * ui_row_centre(width, h), then a widget at ui_next(). */
typedef void (*lab_controls_fn)(int width);

typedef struct {
    lab_controls_fn controls;    /* the showing scene's rows, under the pickers */
    lab_controls_fn settings;    /* the lab's own rows, under the scene's */
    const char* const* families; /* the Scene dropdown's entries; none = no Scene row */
    int family_count;
    int family;                  /* index of the showing one */
    const char* const* variants; /* the showing family's scenes, drawn when it has more than one */
    int variant_count;
    int variant; /* index of the showing one */
} lab_menu_screen_state_t;

typedef struct {
    int family_picked;  /* a family the user chose this frame, else -1 */
    int variant_picked; /* a variant likewise */
} lab_menu_screen_result_t;

/* A caption row, `width` wide and centred, above a widget. */
void lab_menu_caption(int width, const char* text);

/* Draws the scene picker, the scene's and the lab's rows, and the closing hint. Caller
 * brackets this with ui_begin()/ui_end*(). */
lab_menu_screen_result_t lab_menu_screen_draw(const lab_menu_screen_state_t* state);

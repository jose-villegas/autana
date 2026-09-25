/*
 * title_screen - the sand app's first screen: a header, START / LOAD SAVES
 * / OPTIONS, and a GUIDE / EXIT footer.
 *
 * title_screen_layout() is pure geometry over the logical canvas, so
 * suite_title_screen.c checks it at both orientations. The drawing reports
 * which button a tap hit; sand_menu.c decides what that means.
 */
#pragma once

#include "microui.h"

#include "apps/sand/sand_menu.h"

#define TITLE_SCREEN_TITLE    "FALLING SAND: CA"
#define TITLE_SCREEN_SUBTITLE "CELLULAR AUTOMATA"

typedef struct {
    mu_Rect header;
    int title_scale; /* the largest that fits the header */
    mu_Rect subtitle;
    mu_Rect footer;
    mu_Rect buttons[SAND_TITLE_BUTTON_COUNT];
} title_screen_layout_t;

void title_screen_layout(int screen_w, int screen_h, title_screen_layout_t* out);

const char* title_screen_label(sand_title_button_t button);

/* Whether `button` has anything behind it yet; one without is drawn muted. */
bool title_screen_button_enabled(sand_title_button_t button);

/* Caller brackets this with ui_begin()/ui_end(). */
sand_title_button_t title_screen_draw(mu_Context* ctx);

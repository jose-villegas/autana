/*
 * sand_menu_screen - the boot-time menu's microui drawing: START plus the
 * QUALITY/COLOUR/DITHER/TWO-CORE options the app applies at the next
 * start_sim(), never mid-visit. Labels arrive pre-formatted and each
 * option row is a plain bool - docs/Building-a-Screen.md's own split.
 */
#pragma once

#include <stdbool.h>

#include "microui.h"

/* One button's label per option row - already formatted ("QUALITY: Nice"),
 * since the tables behind them are app_sand.c's own. `dither`/`two_core`
 * are read only when `show_dither`/`show_two_core` are set. */
typedef struct {
    const char* quality;
    const char* color;
    const char* dither;
    const char* two_core;
    bool show_dither;
    bool show_two_core;
} sand_menu_screen_state_t;

/* Which button, if any, this frame's tap landed on - app_sand.c applies at
 * most one of these to its own state after the call returns. */
typedef struct {
    bool start_clicked;
    bool quality_clicked;
    bool color_clicked;
    bool dither_clicked;
    bool two_core_clicked;
} sand_menu_screen_result_t;

/* How many rows this menu draws - QUALITY/COLOUR/START plus DITHER and
 * TWO-CORE as their own bools ask. Exposed so sand_menu_screen_start_rect()
 * and this file's own drawing agree on the same centring. */
int sand_menu_screen_row_count(bool show_dither, bool show_two_core);

/* The START button's own on-screen rect, for a device self-test that taps
 * it directly rather than through a real finger - see
 * sand_app_test_start_button_survives_the_ui_build() in app_sand.c. */
mu_Rect sand_menu_screen_start_rect(bool show_dither, bool show_two_core);

/* Draws every row and reports which button this frame's tap landed on.
 * Caller brackets this with ui_begin()/ui_end(). */
sand_menu_screen_result_t sand_menu_screen_draw(mu_Context* ctx, const sand_menu_screen_state_t* state);

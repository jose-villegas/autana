/*
 * sand_menu_screen - the boot-time menu's microui drawing: START plus the
 * QUALITY/COLOUR/DITHER option buttons the app applies at the next
 * start_sim(), never mid-visit.
 *
 * Labels arrive pre-formatted and DITHER's row is a plain bool - this file
 * never sees quality_t, sand_color_mode_t or gfx_dither_mode_t, only what a
 * screen needs: strings and whether a fourth row exists. app_sand.c owns
 * those enums and their name tables and decides what each result field
 * means, the split docs/Building-a-Screen.md asks every screen to keep.
 */
#pragma once

#include <stdbool.h>

#include "microui.h"

/* One button's label per option row - already formatted ("QUALITY: Nice"),
 * since the tables behind them (quality_t, sand_color_mode_t's names,
 * gfx_dither_mode_t's names) are app_sand.c's own. `dither` is read only
 * when `show_dither` is set. */
typedef struct {
    const char* quality;
    const char* color;
    const char* dither;
    bool show_dither;
} sand_menu_screen_state_t;

/* Which button, if any, this frame's tap landed on - app_sand.c applies at
 * most one of these to its own state after the call returns. */
typedef struct {
    bool start_clicked;
    bool quality_clicked;
    bool color_clicked;
    bool dither_clicked;
} sand_menu_screen_result_t;

/* How many rows this menu draws for `show_dither` - QUALITY/COLOUR/START
 * plus DITHER once COLOUR is 16. Exposed so sand_menu_screen_start_rect()
 * and this file's own drawing agree on the same centring without either
 * recomputing it differently. */
int sand_menu_screen_row_count(bool show_dither);

/* The START button's own on-screen rect, for a device self-test that taps
 * it directly rather than through a real finger - see
 * sand_app_test_start_button_survives_the_ui_build() in app_sand.c. */
mu_Rect sand_menu_screen_start_rect(bool show_dither);

/* Draws every row and reports which button this frame's tap landed on.
 * Caller brackets this with ui_begin()/ui_end(). */
sand_menu_screen_result_t sand_menu_screen_draw(mu_Context* ctx, const sand_menu_screen_state_t* state);

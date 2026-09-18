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
#if CONFIG_LAUNCHER_DEVELOPMENT
    /* Pre-formatted like the rows above ("TWO-CORE: On"). */
    const char* two_core;
    /* Current value of the seam-overlay checkbox below - see
     * app_sand.c's draw_seam_overlay(). Development builds only, unlike
     * show_dither: this is a debug aid, not something a release menu
     * offers. */
    bool seam_overlay_on;
#endif
} sand_menu_screen_state_t;

/* Which button, if any, this frame's tap landed on - app_sand.c applies at
 * most one of these to its own state after the call returns. */
typedef struct {
    bool start_clicked;
    bool quality_clicked;
    bool color_clicked;
    bool dither_clicked;
#if CONFIG_LAUNCHER_DEVELOPMENT
    bool two_core_clicked;
    /* Not a click event like the four above - mu_checkbox() mutates this
     * immediately, so it is the checkbox's new value every frame. */
    bool seam_overlay_on;
#endif
} sand_menu_screen_result_t;

/* How many rows this menu draws for `show_dither` - QUALITY/COLOUR/START
 * plus DITHER once COLOUR is 16, plus the TWO-CORE row and the seam-overlay
 * checkbox on a development build. Exposed so sand_menu_screen_row_rect() and this
 * file's own drawing agree on the same row count without either
 * recomputing it differently. */
int sand_menu_screen_row_count(bool show_dither);

/* Row `row` (0-based) of `rows` total, at rest (the window's scroll at 0) -
 * a pure function of the row count so it can be checked well past whatever
 * count `show_dither`/CONFIG_LAUNCHER_DEVELOPMENT ever combine to draw.
 * Once the stack no longer fits the screen, only row 0 is guaranteed to
 * sit at this rect on screen; the rest scroll with the window. */
mu_Rect sand_menu_screen_row_rect(int row, int rows);

/* Row 0's rect - the START button - for a device self-test that taps it
 * directly rather than through a real finger, right after entering the
 * menu, so the window's scroll is still 0 and this rect is exactly where
 * it draws. See sand_app_test_start_button_survives_the_ui_build() in
 * app_sand.c. */
mu_Rect sand_menu_screen_start_rect(bool show_dither);

/* Draws every row and reports which button this frame's tap landed on.
 * Caller brackets this with ui_begin()/ui_end(). */
sand_menu_screen_result_t sand_menu_screen_draw(mu_Context* ctx, const sand_menu_screen_state_t* state);

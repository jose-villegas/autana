/*
 * palette_screen - the material-picker overlay's microui drawing.
 *
 * Geometry lives in palette.h (grid arithmetic, hit rects) and behaviour in
 * sand_ui.h (what a tile tap means); this file only turns a tile index and
 * `ui`'s own brush/mode state into mu_button()/mu_draw_rect() calls, the
 * split docs/Building-a-Screen.md asks every screen to keep.
 */
#pragma once

#include "microui.h"

#include "apps/sand/sand_ui.h"

/* Draws every brush tile and applies a tap to `ui` before returning - see
 * sand_ui_tile_clicked(). Caller brackets this with ui_begin()/
 * ui_end(UI_NO_BACKGROUND). */
void palette_screen_draw(mu_Context* ctx, sand_ui_t* ui);

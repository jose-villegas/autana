/*
 * options_screen - the sand app's launch options: QUALITY as a slider,
 * COLOR MODE as three tiles, a DITHER dropdown once the mode is 16
 * colours, and APPLY / CANCEL pinned to the bottom.
 *
 * Fits both orientations without scrolling: the dropdown's list opens over
 * the screen rather than taking rows in it. The drawing reports hits;
 * sand_menu.c turns them into draft edits.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "microui.h"

#include "apps/sand/sand_menu.h"
#include "apps/sand/sand_mode_swatches.h"

#define OPTIONS_SCREEN_TITLE      "OPTIONS"
#define OPTIONS_SCREEN_QUALITY    "QUALITY"
#define OPTIONS_SCREEN_COLOR_MODE "COLOR MODE"
#define OPTIONS_SCREEN_DITHER     "DITHER"
#define OPTIONS_SCREEN_CANCEL     "CANCEL"

#define OPTIONS_SCREEN_TILE_COUNT 3

/* The app's own option names and colours, handed in so this file never
 * sees its tables. `quality_names` runs finest first; `dither_names` in
 * icons_dither.h's order, so name i shows swatch i; `mode_swatches` is
 * indexed by sand_colour_mode_t. */
typedef struct {
    const char* const* quality_names;
    int quality_count;
    const char* const* dither_names;
    int dither_count;
    const sand_mode_swatch_t* mode_swatches;
} options_screen_labels_t;

typedef struct {
    mu_Rect header;
    mu_Rect quality_panel;
    mu_Rect quality_caption;
    mu_Rect quality_value;
    mu_Rect quality_slider;
    mu_Rect color_caption;
    mu_Rect tiles[OPTIONS_SCREEN_TILE_COUNT];
    mu_Rect dither_caption;
    mu_Rect dither;
    mu_Rect apply;
    mu_Rect cancel;
} options_screen_layout_t;

void options_screen_layout(int screen_w, int screen_h, options_screen_layout_t* out);

/* Tiles run 16, 256, FULL left to right, as the design has them. */
sand_colour_mode_t options_screen_tile_colour(int tile);
const char* options_screen_tile_label(int tile);

/* "APPLY" alone when nothing is pending, "APPLY (n)" otherwise. */
void options_screen_apply_label(int pending, char* out, int len);

/* The slider runs coarse to fine left to right, the reverse of the
 * quality table, so dragging right always means more detail. */
int options_screen_slider_from_quality(int quality, int quality_count);
int options_screen_quality_from_slider(int slider, int quality_count);

/* Caller brackets this with ui_begin()/ui_end(). */
sand_options_hits_t options_screen_draw(mu_Context* ctx, const sand_menu_t* menu,
                                        const options_screen_labels_t* labels);

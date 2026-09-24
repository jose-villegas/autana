/*
 * sand_menu - what the sand app's title and options screens MEAN, with no
 * drawing and no hardware: which of the two is up, what a tap on either
 * does, and the launch options the options screen edits.
 *
 * The options screen edits a draft, never the options a run starts with.
 * APPLY commits the draft and CANCEL drops it, so a half-made choice can
 * never leak into the next START. The screens report what was hit; this
 * decides what it means, so suite_sand_menu.c can pin it on a host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "apps/sand/sand_colour_state.h"

typedef enum { SAND_MENU_TITLE, SAND_MENU_OPTIONS } sand_menu_screen_t;

/* `quality` indexes the app's own quality table, finest first. */
typedef struct {
    int quality;
    sand_colour_mode_t color;
    int dither;
} sand_options_t;

typedef struct {
    sand_menu_screen_t screen;
    sand_options_t committed;
    sand_options_t draft;
} sand_menu_t;

typedef enum {
    SAND_TITLE_NONE = -1,
    SAND_TITLE_START,
    SAND_TITLE_LOAD,
    SAND_TITLE_OPTIONS,
    SAND_TITLE_GUIDE,
    SAND_TITLE_EXIT,
    SAND_TITLE_BUTTON_COUNT,
} sand_title_button_t;

/* What the app must do once a title tap has been decided. */
typedef enum { SAND_MENU_STAY, SAND_MENU_START, SAND_MENU_EXIT } sand_menu_action_t;

/* One frame of the options screen: whichever control a tap landed on.
 * Each index is -1 when its control was not hit. */
typedef struct {
    int quality;
    int color;
    int dither;
    bool apply;
    bool cancel;
} sand_options_hits_t;

#define SAND_OPTIONS_NO_HITS ((sand_options_hits_t){.quality = -1, .color = -1, .dither = -1})

void sand_menu_init(sand_menu_t* menu, sand_options_t current);

/* LOAD SAVES and GUIDE have no screen behind them yet, so they stay put. */
sand_menu_action_t sand_menu_title_clicked(sand_menu_t* menu, sand_title_button_t button);

/* Applies one frame's hits to the draft. Returns true when APPLY committed
 * it, the one moment the app must adopt `committed`. */
bool sand_menu_options_step(sand_menu_t* menu, sand_options_hits_t hits);

/* The draft's differences from `committed` that the screen still shows.
 * A dither pick stops counting once the colour mode no longer uses it. */
int sand_menu_pending_changes(const sand_menu_t* menu);

/* Whether the dither choice means anything under colour mode `color`. */
bool sand_menu_dither_applies(sand_colour_mode_t color);

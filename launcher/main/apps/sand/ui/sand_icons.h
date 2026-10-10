/*
 * sand_icons: the app's icons and dither swatches, one icon set read from
 * the app's own pack (sand.pack.toml's folder). Loaded when the app enters
 * and released when it exits; without the pack every icon is NULL, so a
 * control with a label shows the label alone and an icon-only one its text.
 */
#pragma once

#include "gfx/present/gfx_indexed.h"
#include "ui/ui_icons.h"

#define SAND_PACK "sand"

typedef enum {
    SAND_ICON_POUR,
    SAND_ICON_ERASE,
    SAND_ICON_BOOM,
    SAND_ICON_INFO,
    SAND_ICON_START,
    SAND_ICON_LOAD,
    SAND_ICON_OPTIONS,
    SAND_ICON_GUIDE,
    SAND_ICON_EXIT,
    SAND_ICON_DITHER_FIRST, /* then one swatch per gfx_dither_mode_t, in its order */
    SAND_ICON_COUNT = SAND_ICON_DITHER_FIRST + GFX_DITHER_MODE_COUNT
} sand_icon_id_t;

/* The app's icon set, for ui_icon_set_load(), ui_icon_set_release() and
 * ui_icon_set_draw(). */
extern ui_icon_set_t sand_icon_set;

/* Icon `id`, or NULL without the pack. */
const gfx_image_t* sand_icon(sand_icon_id_t id);

/* The swatch of dither mode `mode`, or NULL without the pack. */
const gfx_image_t* sand_dither_icon(gfx_dither_mode_t mode);

/*
 * sand_icons: the app's icons and dither swatches, read from the app's own
 * pack (sand.pack.toml's folder). Loaded when the app enters and released
 * when it exits; without the pack every icon is NULL, so a control with a
 * label shows the label alone and an icon-only one shows a short text.
 */
#pragma once

#include "gfx/draw/icon.h"
#include "gfx/present/gfx_indexed.h"

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
    SAND_ICON_COUNT
} sand_icon_id_t;

/* Mounts the pack and finds every icon; one log line when it cannot. */
void sand_icons_load(void);

/* Drops what sand_icons_load() holds; every icon is NULL after it. */
void sand_icons_release(void);

/* Icon `id`, or NULL without the pack. */
const gfx_image_t* sand_icon(sand_icon_id_t id);

/* The swatch of dither mode `mode`, or NULL without the pack. */
const gfx_image_t* sand_dither_icon(gfx_dither_mode_t mode);

#include "sand_icons.h"

#include <stdbool.h>

#include "asset/asset_store.h"
#include "ui/ui_icons.h"

static const char* const ICON_NAMES[SAND_ICON_COUNT] = {
    [SAND_ICON_POUR] = "pour",       [SAND_ICON_ERASE] = "erase", [SAND_ICON_BOOM] = "boom",
    [SAND_ICON_INFO] = "brush_info", [SAND_ICON_START] = "start", [SAND_ICON_LOAD] = "load",
    [SAND_ICON_OPTIONS] = "options", [SAND_ICON_GUIDE] = "guide", [SAND_ICON_EXIT] = "exit",
};

static const char* const DITHER_NAMES[GFX_DITHER_MODE_COUNT] = {
    [GFX_DITHER_NONE] = "dither_none",
    [GFX_DITHER_CELL_CHECKER] = "dither_cell_checker",
    [GFX_DITHER_CELL_BAYER2] = "dither_cell_bayer2",
    [GFX_DITHER_PIXEL_CHECKER2] = "dither_pixel_checker2",
    [GFX_DITHER_PIXEL_BAYER4] = "dither_pixel_bayer4",
};

static gfx_image_t icons[SAND_ICON_COUNT];
static gfx_image_t dither[GFX_DITHER_MODE_COUNT];
static bool icons_loaded;
static bool dither_loaded;

void
sand_icons_load(void) {
    if (!icons_loaded) {
        icons_loaded = ui_icons_load(SAND_PACK, ICON_NAMES, SAND_ICON_COUNT, icons);
    }
    /* After the icons only, so a missing pack is one log line, not two. */
    if (icons_loaded && !dither_loaded) {
        dither_loaded = ui_icons_load(SAND_PACK, DITHER_NAMES, GFX_DITHER_MODE_COUNT, dither);
    }
}

void
sand_icons_release(void) {
    if (icons_loaded) {
        asset_store_release(SAND_PACK);
    }
    if (dither_loaded) {
        asset_store_release(SAND_PACK);
    }
    icons_loaded = false;
    dither_loaded = false;
}

const gfx_image_t*
sand_icon(sand_icon_id_t id) {
    return icons_loaded ? &icons[id] : NULL;
}

const gfx_image_t*
sand_dither_icon(gfx_dither_mode_t mode) {
    return dither_loaded ? &dither[mode] : NULL;
}

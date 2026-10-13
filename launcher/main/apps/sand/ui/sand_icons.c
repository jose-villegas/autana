#include "sand_icons.h"

static const ui_icon_name_t NAMES[SAND_ICON_COUNT] = {
    [SAND_ICON_POUR] = {"pour", ""},
    [SAND_ICON_ERASE] = {"erase", ""},
    [SAND_ICON_BOOM] = {"boom", ""},
    [SAND_ICON_INFO] = {"brush_info", "i"},
    [SAND_ICON_START] = {"start", ""},
    [SAND_ICON_LOAD] = {"load", ""},
    [SAND_ICON_OPTIONS] = {"options", ""},
    [SAND_ICON_GUIDE] = {"guide", ""},
    [SAND_ICON_EXIT] = {"exit", ""},
    [SAND_ICON_DITHER_FIRST + GFX_DITHER_NONE] = {"dither_none", ""},
    [SAND_ICON_DITHER_FIRST + GFX_DITHER_CELL_CHECKER] = {"dither_cell_checker", ""},
    [SAND_ICON_DITHER_FIRST + GFX_DITHER_CELL_BAYER2] = {"dither_cell_bayer2", ""},
    [SAND_ICON_DITHER_FIRST + GFX_DITHER_PIXEL_CHECKER2] = {"dither_pixel_checker2", ""},
    [SAND_ICON_DITHER_FIRST + GFX_DITHER_PIXEL_BAYER4] = {"dither_pixel_bayer4", ""},
};

static gfx_image_t icons[SAND_ICON_COUNT];

ui_icon_set_t sand_icon_set = {SAND_PACK, NAMES, SAND_ICON_COUNT, icons, false};

const gfx_image_t*
sand_icon(sand_icon_id_t id) {
    return ui_icon_set_get(&sand_icon_set, (int)id);
}

const gfx_image_t*
sand_dither_icon(gfx_dither_mode_t mode) {
    return ui_icon_set_get(&sand_icon_set, SAND_ICON_DITHER_FIRST + (int)mode);
}

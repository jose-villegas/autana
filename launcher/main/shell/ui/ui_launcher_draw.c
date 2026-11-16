/*
 * ui_launcher_draw: the home screen's own rows. See ui_launcher.c for what
 * this is split out of and why: the framebuffer, and so ui_end(), belongs
 * to that file alone, which is what lets this half link and run on a host.
 */

#include "shell/ui/ui_launcher.h"

#include "app/app.h"
#include "asset/asset_store.h"
#include "display/display.h"
#include "draw/draw_sprite.h"
#include "esp_log.h"
#include "ui/ui.h"

static const char* TAG = "ui_launcher";

/* The grid's shape held each way: wider than tall gets the extra column,
 * taller than wide the extra row. Apps past one screen scroll. */
#define LAUNCHER_COLS_LANDSCAPE     3
#define LAUNCHER_ROWS_LANDSCAPE     2
#define LAUNCHER_COLS_PORTRAIT      2
#define LAUNCHER_ROWS_PORTRAIT      3
/* The tile faces' engine pack entries. */
#define LAUNCHER_TILE_ENTRY         "home_tile"
#define LAUNCHER_TILE_PRESSED_ENTRY "home_tile_pressed"
/* The grid window's inset, so with the window's own padding the tiles keep
 * the panel's touch-safe margin. */
#define LAUNCHER_GRID_INSET         (DISPLAY_PANEL_SAFE_INSET - UI_PADDING)

/* The tiles' skin; its sprites point into the engine pack, kept loaded for
 * them. Without them the tiles keep the code-drawn faces. */
static sprite_t tile_face;
static sprite_t tile_face_pressed;
static ui_skin_t tile_skin;

static void
open_tile_skin(void) {
    if (tile_skin.normal != NULL) {
        return;
    }
    const handle_t load = asset_store_load(ASSET_ENGINE_PACK);
    const asset_pack_t* pack = asset_store_loaded(load);
    asset_status_t status = pack != NULL ? draw_sprite_open(pack, LAUNCHER_TILE_ENTRY, &tile_face) : ASSET_ERR_NO_PACK;
    if (status == ASSET_OK) {
        status = draw_sprite_open(pack, LAUNCHER_TILE_PRESSED_ENTRY, &tile_face_pressed);
    }
    if (status != ASSET_OK) {
        asset_store_release(load);
        ESP_LOGW(TAG, "no tile skin in pack %s (%s): tiles are drawn", ASSET_ENGINE_PACK, asset_status_text(status));
        return;
    }
    tile_skin = (ui_skin_t){.normal = &tile_face, .pressed = &tile_face_pressed};
}

void
ui_launcher_init(void) {
    ui_init();
    open_tile_skin();
}

/* The grid fills the window below the banner strip, which is left blank: its width shared out across
 * the columns, its height across the rows. Centred while it fits, scrolling
 * once it does not. Returns which app this frame's tap chose, or NULL. */
static const app_t*
draw_app_grid(void) {
    const recti_t window = {LAUNCHER_GRID_INSET, UI_BANNER_HEIGHT, ui_width() - (2 * LAUNCHER_GRID_INSET),
                            ui_height() - UI_BANNER_HEIGHT - LAUNCHER_GRID_INSET};
    const bool landscape = ui_width() > ui_height();
    const int rows = landscape ? LAUNCHER_ROWS_LANDSCAPE : LAUNCHER_ROWS_PORTRAIT;

    ui_scroll_window_aligned(window, UI_ALIGN_CENTRE);
    ui_skin(tile_skin.normal != NULL ? &tile_skin : NULL);
    ui_row_cells(landscape ? LAUNCHER_COLS_LANDSCAPE : LAUNCHER_COLS_PORTRAIT, ui_cell_height(rows));
    const app_t* chosen = NULL;
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        if (ui_button(ui_next(), app->name)) {
            chosen = app;
        }
    }
    ui_skin(NULL);
    return chosen;
}

const app_t*
ui_launcher_draw(void) {
    /* Sized from ui_width()/ui_height(), not GFX_WIDTH/GFX_HEIGHT, so it
     * still fits after a quarter turn swaps which one is larger. */
    return draw_app_grid();
}

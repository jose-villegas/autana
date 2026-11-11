/*
 * ui_launcher_draw: the home screen's own rows. See ui_launcher.c for what
 * this is split out of and why: the framebuffer, and so ui_end(), belongs
 * to that file alone, which is what lets this half link and run on a host.
 */

#include "shell/ui/ui_launcher.h"

#include "app/app.h"
#include "display/display.h"
#include "ui/ui.h"

/* The grid's shape held each way: wider than tall gets the extra column,
 * taller than wide the extra row. Apps past one screen scroll. */
#define LAUNCHER_COLS_LANDSCAPE 3
#define LAUNCHER_ROWS_LANDSCAPE 2
#define LAUNCHER_COLS_PORTRAIT  2
#define LAUNCHER_ROWS_PORTRAIT  3
/* Soft corners on flat tiles, inside the panel's own corner. */
#define LAUNCHER_TILE_RADIUS    16
/* The grid window's inset, so with the window's own padding the tiles keep
 * the panel's touch-safe margin. */
#define LAUNCHER_GRID_INSET     (DISPLAY_PANEL_SAFE_INSET - UI_PADDING)

void
ui_launcher_init(void) {
    ui_init();
}

/* The strip across the top, reserved for status and left blank. */
static void
draw_banner(void) {
    ui_window((recti_t){0, 0, ui_width(), UI_BANNER_HEIGHT});
}

/* The grid fills the window below the banner: its width shared out across
 * the columns, its height across the rows. Centred while it fits, scrolling
 * once it does not. Returns which app this frame's tap chose, or NULL. */
static const app_t*
draw_app_grid(void) {
    const recti_t window = {LAUNCHER_GRID_INSET, UI_BANNER_HEIGHT, ui_width() - (2 * LAUNCHER_GRID_INSET),
                            ui_height() - UI_BANNER_HEIGHT - LAUNCHER_GRID_INSET};
    const bool landscape = ui_width() > ui_height();
    const int rows = landscape ? LAUNCHER_ROWS_LANDSCAPE : LAUNCHER_ROWS_PORTRAIT;
    const int cell_h = (window.h - (2 * UI_PADDING) - ((rows - 1) * UI_ROW_GAP)) / rows;

    ui_scroll_window_aligned(window, UI_ALIGN_CENTRE);
    ui_corner_radius(LAUNCHER_TILE_RADIUS);
    ui_row_cells(landscape ? LAUNCHER_COLS_LANDSCAPE : LAUNCHER_COLS_PORTRAIT, cell_h);
    const app_t* chosen = NULL;
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        if (ui_button(ui_next(), app->name)) {
            chosen = app;
        }
    }
    ui_corner_radius(0);
    return chosen;
}

const app_t*
ui_launcher_draw(void) {
    /* Sized from ui_width()/ui_height(), not GFX_WIDTH/GFX_HEIGHT, so it
     * still fits after a quarter turn swaps which one is larger. */
    draw_banner();
    return draw_app_grid();
}

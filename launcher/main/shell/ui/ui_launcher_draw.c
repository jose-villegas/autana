/*
 * ui_launcher_draw: the home screen's own rows. See ui_launcher.c for what
 * this is split out of and why: the framebuffer, and so ui_end(), belongs
 * to that file alone, which is what lets this half link and run on a host.
 */

#include "shell/ui/ui_launcher.h"

#include "app/app.h"
#include "ui/ui.h"

/* A label too wide is clipped at both ends with no warning, so this is
 * sized to the longest registered name (192 px) plus slack. Check a new
 * name's width before adding it. */
#define LAUNCHER_BTN_W 240

void
ui_launcher_init(void) {
    ui_init();
}

/* Claimed from the flow and left blank, reserving the strip for future
 * status with no widget drawn into it yet: UI_BANNER_HEIGHT from the top of
 * the screen, of which the window's own padding is the first part. */
static void
draw_banner(void) {
    ui_row(UI_BANNER_HEIGHT - UI_PADDING);
    (void)ui_next();
}

/* Fixed-width, centred not filled. FLOWED, not placed at an absolute y, so
 * enough registered apps overflow into the window's own scroll instead of
 * pushing later entries off screen with no way back to them. Returns which
 * app this frame's tap chose, or NULL. */
static const app_t*
draw_app_rows(void) {
    const app_t* chosen = NULL;
    for (const app_t* app = app_list(); app != NULL; app = app->next) {
        ui_row_centre(LAUNCHER_BTN_W, UI_ROW_HEIGHT);
        if (ui_button(ui_next(), app->name)) {
            chosen = app;
        }
    }
    return chosen;
}

const app_t*
ui_launcher_draw(void) {
    /* One full-screen window with no chrome, sized from
     * ui_width()/ui_height() rather than GFX_WIDTH/GFX_HEIGHT so it still
     * fits after a quarter turn swaps which one is larger. */
    ui_scroll_window((recti_t){0, 0, ui_width(), ui_height()});
    draw_banner();
    return draw_app_rows();
}

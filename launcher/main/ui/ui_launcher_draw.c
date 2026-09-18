/*
 * ui_launcher_draw - the home screen's own rows. See ui_launcher.c for what
 * this is split out of and why: the framebuffer, and so ui_end(), belongs
 * to that file alone, which is what lets this half link and run on a host.
 */

#include "ui/ui_launcher.h"

#include "app.h"
#include "ui/ui.h"
#include "ui/ui_scroll.h"

/* A label too wide is clipped at both ends with no warning, so this is
 * sized to the longest registered name (192 px) plus slack. Check a new
 * name's width before adding it. */
#define LAUNCHER_BTN_W 240

void
ui_launcher_init(void) {
    ui_init();
}

/* Claimed from the layout and left blank: mu_layout_next() hands back the
 * rect and advances past it, reserving the space for future status with no
 * widget drawn into it yet. */
static void
draw_banner(mu_Context* ctx) {
    mu_layout_row(ctx, 1, (int[]){-1}, UI_BANNER_HEIGHT);
    mu_layout_next(ctx);
}

/* Fixed-width, centred not filled - see ui_centered_rect(). FLOWED, not
 * placed at an absolute y, so enough registered apps now overflow into the
 * window's own scroll instead of pushing later entries off screen with no
 * way back to them - see ui_scroll.h. Returns which app this frame's tap
 * chose, or -1. */
static int
draw_app_rows(mu_Context* ctx) {
    int chosen = -1;
    ui_flow_t flow = ui_flow_start(ui_width(), UI_BANNER_HEIGHT + UI_ROW_GAP, UI_ROW_GAP);
    for (int i = 0; i < app_list_count(); i++) {
        ui_flow_row(ctx, &flow, LAUNCHER_BTN_W, UI_ROW_HEIGHT);
        if (mu_button(ctx, app_list()[i]->name)) {
            chosen = i;
        }
    }
    return chosen;
}

int
ui_launcher_draw(mu_Context* ctx, uint32_t dt_ms) {
    /* Bezelled buttons, stated every frame because style does not persist
     * - see ui.h. One full-screen window with no chrome, sized from
     * ui_width()/ui_height() rather than GFX_WIDTH/GFX_HEIGHT so it still
     * fits after a quarter turn swaps which one is larger. */
    ui_set_button_style(UI_BUTTON_BEZEL);

    const int opt = MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME;
    if (!ui_scroll_view_begin(ctx, "Launcher", opt, ui_scroll_view_default(), dt_ms)) {
        return -1;
    }

    draw_banner(ctx);
    const int chosen = draw_app_rows(ctx);
    ui_scroll_view_end(ctx);
    return chosen;
}

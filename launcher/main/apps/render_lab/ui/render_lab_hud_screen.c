#include "render_lab_hud_screen.h"

#include <stdio.h>

#include "gfx/gfx.h"
#include "ui/ui.h"

/* Mirrors app_render_lab.c's own BACKGROUND_RGB - kept as this file's own
 * copy rather than shared, since a copy this small is not worth a shared
 * header of its own. */
#define RENDER_LAB_HUD_BACKGROUND_RGB 0x0A0C14

static mu_Color
mu_color_hex(uint32_t rgb) {
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF), 255);
}

/* mu_text() draws only its own ink. Under partial_updates a scene only
 * erases its OWN last bounding box, never this one, so old text the
 * repainted line doesn't overdraw would stay on screen without this opaque
 * backing - painted through the same mu command list already hashed, at
 * no cost on frames where nothing changed. */
static mu_Rect
draw_overlay_box(mu_Context* ctx, int w, int h, int x_override, int y) {
    mu_Rect box = ui_centered_rect(ui_width(), w, h, y);
    if (x_override >= 0) {
        box.x = x_override;
    }
    mu_draw_rect(ctx, box, mu_color_hex(RENDER_LAB_HUD_BACKGROUND_RGB));
    return box;
}

/* Appended to the fps line, a scene's vertex/edge text can overflow the
 * panel width - so it gets its own narrower box stacked right below,
 * leaving the fps line above at the position it already had. */
#define STATUS_GAP_PX 4

static void
draw_status_line(mu_Context* ctx, const char* status, mu_Rect fps_box) {
    if (status == NULL) {
        return;
    }
    const int sw = gfx_text_width(status, -1);
    const int sh = gfx_text_height() + 4;
    const mu_Rect box = draw_overlay_box(ctx, sw + 8, sh, -1, fps_box.y + fps_box.h + STATUS_GAP_PX);
    mu_layout_set_next(ctx, box, 0);
    mu_text(ctx, status);
}

void
render_lab_hud_screen_draw(mu_Context* ctx, const render_lab_hud_screen_state_t* state) {
    /* UI_TEXT_OUTLINED is app_sand.c's palette-label fix: a label with no
     * halo would wash out against whichever shifting colour sits behind
     * it. Kept even with the box's opaque backing - a NO_BACKGROUND window
     * is one BOOT tap away - since the halo costs nothing extra there. */
    ui_set_text_style(UI_TEXT_OUTLINED);

    if (ui_begin_screen(ctx, "Render Lab HUD", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        char fps_line[48];
        snprintf(fps_line, sizeof fps_line, "%s  %.1f fps", state->scene_name, state->fps_value);
        const int tw = gfx_text_width(fps_line, -1);
        const int th = gfx_text_height() + 4;

        const mu_Rect box = draw_overlay_box(ctx, tw + 8, th, state->fps_box_x_override, 2);
        mu_layout_set_next(ctx, box, 0);
        mu_text(ctx, fps_line);

        draw_status_line(ctx, state->status, box);

        mu_end_window(ctx);
    }
}

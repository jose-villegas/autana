#include "cube_hud_screen.h"

#include <stdio.h>

#include "gfx/gfx.h"
#include "ui/ui.h"

/* Mirrors app_cube.c's own BACKGROUND_RGB - kept as this file's own copy
 * rather than shared, since a copy this small is not worth a shared
 * header of its own. */
#define CUBE_HUD_BACKGROUND_RGB 0x0A0C14

static mu_Color
mu_color_hex(uint32_t rgb) {
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF), 255);
}

/* BUT: mu_text() draws only its own ink, no background of its own. Under
 * partial_updates, cube_frame() only erases the CUBE's own last bounding
 * box, never this box's, so when the fps line repaints - it changes shape
 * every FPS_WINDOW_MS as digits change - old digits the new ones don't
 * overdraw stay on screen. An opaque box behind the text, painted through
 * the same mu command list already hashed, fixes this at no cost on
 * frames where fps did not change. */
static mu_Rect
draw_overlay_box(mu_Context* ctx, int w, int h, int x_override) {
    mu_Rect box = ui_centered_rect(ui_width(), w, h, 2);
    if (x_override >= 0) {
        box.x = x_override;
    }
    mu_draw_rect(ctx, box, mu_color_hex(CUBE_HUD_BACKGROUND_RGB));
    return box;
}

void
cube_hud_screen_draw(mu_Context* ctx, const cube_hud_screen_state_t* state) {
    /* UI_TEXT_OUTLINED is app_sand.c's palette-label fix for the same
     * reason it was built for: a label with no halo of its own would wash
     * out against whichever of the cube's shifting corner colours happens
     * to sit behind it. Left in place even with the box's own opaque
     * backing - a NO_BACKGROUND window is still one BOOT tap away whenever
     * partial_updates is off, and the halo costs nothing extra when the
     * backing is already opaque. */
    ui_set_text_style(UI_TEXT_OUTLINED);

    if (ui_begin_screen(ctx, "Cube HUD", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        char fps_line[16];
        snprintf(fps_line, sizeof fps_line, "%.1f fps", state->fps_value);
        const int tw = gfx_text_width(fps_line, -1);
        const int th = gfx_text_height() + 4;

        const mu_Rect box = draw_overlay_box(ctx, tw + 8, th, state->fps_box_x_override);
        mu_layout_set_next(ctx, box, 0);
        mu_text(ctx, fps_line);

        mu_end_window(ctx);
    }
}

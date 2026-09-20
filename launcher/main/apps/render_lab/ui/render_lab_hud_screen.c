#include "render_lab_hud_screen.h"

#include <stdio.h>

#include "../render_lab.h"
#include "gfx/gfx.h"
#include "ui/ui.h"

static mu_Color
mu_color_hex(uint32_t rgb) {
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF), 255);
}

/* The panel's bezel hides about 15 px along every edge. */
#define HUD_INSET_PX    16
#define HUD_LINE_GAP_PX 4

/* mu_text() draws only its own ink. Under render_lab_partial_updates a scene only
 * erases its OWN last bounding box, never this one, so old text the
 * repainted line doesn't overdraw would stay on screen without this opaque
 * backing - painted through the same mu command list already hashed, at
 * no cost on frames where nothing changed. */
static mu_Rect
draw_text_box(mu_Context* ctx, const char* text, int x, int y) {
    const mu_Rect box = mu_rect(x, y, gfx_text_width(text, -1) + 8, gfx_text_height() + 4);
    mu_draw_rect(ctx, box, mu_color_hex(RENDER_LAB_BACKGROUND_RGB));
    mu_layout_set_next(ctx, box, 0);
    mu_text(ctx, text);
    return box;
}

/* Centred on the highest HUD row whose corner box it clears: `row_right[i]`
 * is where row i's box ends, 0 for an empty row. The box stays opaque while
 * the ink fades: a dithered box would leave the pixels it no longer covers
 * showing last frame's title wherever no scene erases them. */
#define HUD_CORNER_ROWS 2

static void
draw_scene_title(mu_Context* ctx, const char* title, uint8_t alpha, const int row_right[HUD_CORNER_ROWS]) {
    if (alpha == 0) {
        return;
    }
    const int w = gfx_text_width(title, -1) + 8;
    const int h = gfx_text_height() + 4;
    mu_Rect box = ui_centered_rect(ui_width(), w, h, HUD_INSET_PX);

    int row = 0;
    while (row < HUD_CORNER_ROWS && box.x < row_right[row] + HUD_LINE_GAP_PX) {
        row++;
    }
    box.y = HUD_INSET_PX + row * (h + HUD_LINE_GAP_PX);
    mu_draw_rect(ctx, box, mu_color_hex(RENDER_LAB_BACKGROUND_RGB));

    mu_Color ink = ctx->style->colors[MU_COLOR_TEXT];
    ink.a = alpha;
    mu_draw_text(ctx, ctx->style->font, title, -1, mu_vec2(box.x + 4, box.y + 2), ink);
}

void
render_lab_hud_screen_draw(mu_Context* ctx, const render_lab_hud_screen_state_t* state) {
    /* UI_TEXT_OUTLINED is app_sand.c's palette-label fix: a label with no
     * halo would wash out against whichever shifting colour sits behind
     * it. Kept even with the box's opaque backing - a NO_BACKGROUND window
     * is one BOOT tap away - since the halo costs nothing extra there. */
    ui_set_text_style(UI_TEXT_OUTLINED);

    if (ui_begin_screen(ctx, "Render Lab HUD", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        char fps_line[16];
        snprintf(fps_line, sizeof fps_line, "%5.1f fps", state->fps_value);

        const int x = state->fps_box_x_override >= 0 ? state->fps_box_x_override : HUD_INSET_PX;
        const mu_Rect fps_box = draw_text_box(ctx, fps_line, x, HUD_INSET_PX);
        /* The fps row is reserved at its widest reading, so the title does
         * not change rows as the number gains a digit. */
        int row_right[HUD_CORNER_ROWS] = {fps_box.x + gfx_text_width("999.9 fps", -1) + 8, 0};
        if (state->status != NULL) {
            const mu_Rect status_box = draw_text_box(ctx, state->status, x, fps_box.y + fps_box.h + HUD_LINE_GAP_PX);
            row_right[1] = status_box.x + status_box.w;
        }
        draw_scene_title(ctx, state->scene_title, state->scene_title_alpha, row_right);

        mu_end_window(ctx);
    }
}

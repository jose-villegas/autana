#include "palette_screen.h"

#include "gfx/gfx_color.h"
#include "ui/ui.h"
#include "ui/ui_style.h"

#include "apps/sand/material.h"
#include "apps/sand/material_palette.h"
#include "apps/sand/palette.h"

#define PALETTE_GROUT              4
#define PALETTE_BEZEL              3

#define PALETTE_BADGE_SIZE         18
#define PALETTE_BADGE_INSET        2
#define PALETTE_BADGE_MARGIN       2

#define PALETTE_BADGE_BORDER_COLOR 0x141414
#define PALETTE_BADGE_FILL_COLOR   0xF2F2F2

static mu_Color
mu_color_hex(uint32_t rgb) {
    return mu_color((int)((rgb >> 16) & 0xFF), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF), 255);
}

static void
draw_palette_selection_bezel(mu_Context* ctx, mu_Rect r, mu_Color face) {
    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n = ui_bezel_spans(r, face, true, spans, UI_BEZEL_MAX_SPANS);
    for (int s = 1; s < n; s++) {
        mu_draw_rect(ctx, spans[s].rect, spans[s].color);
    }
}

static void
draw_palette_badge(mu_Context* ctx, sand_ui_t* ui, int i, int ix, int iy, int iw) {
    if (!material_can_emit(ui->brushes[i].cell)) {
        return;
    }
    const mu_Color border = mu_color_hex(PALETTE_BADGE_BORDER_COLOR);
    const mu_Color fill = mu_color_hex(PALETTE_BADGE_FILL_COLOR);
    const int bx = ix + iw - PALETTE_BEZEL - PALETTE_BADGE_MARGIN - PALETTE_BADGE_SIZE;
    const int by = iy + PALETTE_BEZEL + PALETTE_BADGE_MARGIN;
    const mu_Rect badge_rect = mu_rect(bx, by, PALETTE_BADGE_SIZE, PALETTE_BADGE_SIZE);

    mu_draw_rect(ctx, badge_rect, border);
    mu_draw_rect(ctx,
                 mu_rect(bx + PALETTE_BADGE_INSET, by + PALETTE_BADGE_INSET,
                         PALETTE_BADGE_SIZE - 2 * PALETTE_BADGE_INSET, PALETTE_BADGE_SIZE - 2 * PALETTE_BADGE_INSET),
                 fill);

    if (ui->modes[i] == BRUSH_SPAWN) {
        mu_draw_icon(ctx, MU_ICON_CHECK, badge_rect, border);
    }
}

static void
draw_palette_tile(mu_Context* ctx, sand_ui_t* ui, int i, int cols) {
    int x, y, w, h;
    palette_tile_rect(i, ui->brush_count, cols, ui_width(), ui_height(), &x, &y, &w, &h);

    const int ix = x + PALETTE_GROUT;
    const int iy = y + PALETTE_GROUT;
    const int iw = w - 2 * PALETTE_GROUT;
    const int ih = h - 2 * PALETTE_GROUT;

    const mu_Color face = mu_color_hex(gfx_color_rgb888(material_brush_color(ui->brushes[i].cell)));
    ctx->style->colors[MU_COLOR_BUTTON] = face;

    const char* name = material_name(ui->brushes[i].cell);
    mu_layout_set_next(ctx, mu_rect(ix, iy, iw, ih), 0);
    const int clicked = mu_button(ctx, name);

    if (clicked) {
        sand_ui_tile_clicked(ui, i);
    }

    /* No "selected" state exists for mu_button(), so this draws a second
     * cue: a SUNKEN edge pair mixed toward white/black off THIS TILE'S face
     * - keeps it visible on both Snow and Stone. Safe here, unlike the
     * badge below: it always sits paired with the one face it was mixed
     * from. */
    if (i == ui->brush) {
        draw_palette_selection_bezel(ctx, mu_rect(ix, iy, iw, ih), face);
    }

    /* Badge shows eligibility (material_can_emit(), false for every
     * KIND_STATIC material except gunpowder's KIND_POWDER). Border/fill are
     * a FIXED pair, not derived from the face like the bezel above - Snow's
     * near-white face would make a derived fill nearly invisible. */
    draw_palette_badge(ctx, ui, i, ix, iy, iw);
}

void
palette_screen_draw(mu_Context* ctx, sand_ui_t* ui) {
    ui_set_text_style(UI_TEXT_OUTLINED);
    ui_set_button_style(UI_BUTTON_BEZEL);

    const mu_Color saved_button_color = ctx->style->colors[MU_COLOR_BUTTON];
    const mu_Color saved_text_color = ctx->style->colors[MU_COLOR_TEXT];
    ctx->style->colors[MU_COLOR_TEXT] = mu_color(0, 0, 0, 255);

    const int cols = palette_cols(ui_width());

    if (ui_begin_screen(ctx, "Sand Palette", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        for (int i = 0; i < ui->brush_count; i++) {
            draw_palette_tile(ctx, ui, i, cols);
        }
        mu_end_window(ctx);
    }

    ctx->style->colors[MU_COLOR_BUTTON] = saved_button_color;
    ctx->style->colors[MU_COLOR_TEXT] = saved_text_color;
}

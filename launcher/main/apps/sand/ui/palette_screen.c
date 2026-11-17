#include "palette_screen.h"

#include "ui/ui.h"

#include "apps/sand/material.h"
#include "apps/sand/material_palette.h"
#include "apps/sand/palette.h"
#include "image/pixel/rgb565.h"
#include "sand_theme.h"

#define PALETTE_GROUT              4
#define PALETTE_BEZEL              3

#define PALETTE_BADGE_SIZE         18
#define PALETTE_BADGE_INSET        2
#define PALETTE_BADGE_MARGIN       2

#define PALETTE_BADGE_BORDER_COLOR 0x141414
#define PALETTE_BADGE_FILL_COLOR   0xF2F2F2
#define PALETTE_LABEL_COLOR        0x000000

static void
draw_palette_badge(sand_ui_t* ui, int i, recti_t tile) {
    if (!material_can_emit(ui->brushes[i].cell)) {
        return;
    }
    const recti_t badge = {
        tile.x + tile.w - PALETTE_BEZEL - PALETTE_BADGE_MARGIN - PALETTE_BADGE_SIZE,
        tile.y + PALETTE_BEZEL + PALETTE_BADGE_MARGIN,
        PALETTE_BADGE_SIZE,
        PALETTE_BADGE_SIZE,
    };
    ui_fill(badge, PALETTE_BADGE_BORDER_COLOR);
    ui_fill((recti_t){badge.x + PALETTE_BADGE_INSET, badge.y + PALETTE_BADGE_INSET,
                      PALETTE_BADGE_SIZE - (2 * PALETTE_BADGE_INSET), PALETTE_BADGE_SIZE - (2 * PALETTE_BADGE_INSET)},
            PALETTE_BADGE_FILL_COLOR);

    if (ui->modes[i] == BRUSH_SPAWN) {
        ui_color_begin(UI_COLOR_TEXT, PALETTE_BADGE_BORDER_COLOR);
        ui_check(badge);
        ui_color_end();
    }
}

static void
draw_palette_tile(sand_ui_t* ui, int i, int cols) {
    int x;
    int y;
    int w;
    int h;
    palette_tile_rect(i, ui->brush_count, cols, ui_width(), ui_height(), &x, &y, &w, &h);
    const recti_t tile = {x + PALETTE_GROUT, y + PALETTE_GROUT, w - (2 * PALETTE_GROUT), h - (2 * PALETTE_GROUT)};

    const uint32_t face = rgb565be_to_rgb888(material_brush_color(ui->brushes[i].cell));
    ui_color_begin(UI_COLOR_FACE, face);
    if (ui_button(tile, material_name(ui->brushes[i].cell))) {
        sand_ui_tile_clicked(ui, i);
    }
    ui_color_end();

    /* The selected tile's second cue: its edges sunk, mixed toward
     * white/black off THIS TILE'S face, so it reads on both Snow and
     * Stone. */
    if (i == ui->brush) {
        ui_bevel(tile, face, true);
    }

    /* Badge shows eligibility (material_can_emit(), false for every
     * KIND_STATIC material except gunpowder's KIND_POWDER). Border/fill are
     * a FIXED pair, not derived from the face like the bevel above - Snow's
     * near-white face would make a derived fill nearly invisible. */
    draw_palette_badge(ui, i, tile);
}

void
palette_screen_draw(sand_ui_t* ui) {
    ui_set_text_style(UI_TEXT_OUTLINED);

    const int cols = palette_cols(ui_width());

    ui_screen();
    ui_theme(&sand_ui_theme);
    ui_color_begin(UI_COLOR_TEXT, PALETTE_LABEL_COLOR);
    for (int i = 0; i < ui->brush_count; i++) {
        draw_palette_tile(ui, i, cols);
    }
    ui_color_end();
}

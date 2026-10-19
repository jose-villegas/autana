#include "options_screen.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/ui.h"

#include "apps/sand/ui/sand_icons.h"
#include "math/scalar/mathi.h"
#include "sand_theme.h"

#define COLUMN_W_MAX    400
#define HEADER_H        36
#define SECTION_GAP     6
#define CAPTION_H       20
#define CAPTION_GAP     6
#define PANEL_PAD       8
#define SLIDER_H        UI_TAP_MIN
#define QUALITY_PANEL_H (PANEL_PAD + CAPTION_H + PANEL_PAD + SLIDER_H + PANEL_PAD)
#define TILE_H          68
#define TILE_GAP        8
#define DROPDOWN_H      UI_TAP_MIN
#define BOTTOM_MARGIN   8
#define BODY_H                                                                                                         \
    (SECTION_GAP + QUALITY_PANEL_H + SECTION_GAP + CAPTION_H + CAPTION_GAP + TILE_H + SECTION_GAP + CAPTION_H          \
     + CAPTION_GAP + DROPDOWN_H + SECTION_GAP + FOOTER_H + BOTTOM_MARGIN)
#define FOOTER_H     UI_TAP_MIN
#define FOOTER_GAP   8
#define SWATCH_INSET 10

static const sand_colour_mode_t TILE_COLOURS[OPTIONS_SCREEN_TILE_COUNT] = {
    SAND_COLOUR_16,
    SAND_COLOUR_256,
    SAND_COLOUR_FULL,
};

static const char* const TILE_LABELS[OPTIONS_SCREEN_TILE_COUNT] = {"16", "256", "FULL"};

sand_colour_mode_t
options_screen_tile_colour(int tile) {
    return TILE_COLOURS[tile];
}

const char*
options_screen_tile_label(int tile) {
    return TILE_LABELS[tile];
}

void
options_screen_apply_label(int pending, char* out, int len) {
    if (pending > 0) {
        snprintf(out, (size_t)len, "APPLY (%d)", pending);
    } else {
        snprintf(out, (size_t)len, "APPLY");
    }
}

int
options_screen_slider_from_quality(int quality, int quality_count) {
    return quality_count - 1 - quality;
}

int
options_screen_quality_from_slider(int slider, int quality_count) {
    return quality_count - 1 - slider;
}

static int
layout_quality(int x, int w, int y, options_screen_layout_t* out) {
    out->quality_panel = (recti_t){x, y, w, QUALITY_PANEL_H};
    const int inner_x = x + PANEL_PAD;
    const int inner_w = w - 2 * PANEL_PAD;
    const int half = inner_w / 2;
    out->quality_caption = (recti_t){inner_x, y + PANEL_PAD, half, CAPTION_H};
    out->quality_value = (recti_t){inner_x + half, y + PANEL_PAD, inner_w - half, CAPTION_H};
    out->quality_slider = (recti_t){inner_x, y + QUALITY_PANEL_H - PANEL_PAD - SLIDER_H, inner_w, SLIDER_H};
    return y + QUALITY_PANEL_H + SECTION_GAP;
}

static int
layout_tiles(int x, int w, int y, options_screen_layout_t* out) {
    out->color_caption = (recti_t){x, y, w, CAPTION_H};
    y += CAPTION_H + CAPTION_GAP;
    const int n = OPTIONS_SCREEN_TILE_COUNT;
    const int tile_w = (w - (n - 1) * TILE_GAP) / n;
    const int start_x = x + (w - (n * tile_w + (n - 1) * TILE_GAP)) / 2;
    for (int i = 0; i < n; i++) {
        out->tiles[i] = (recti_t){start_x + i * (tile_w + TILE_GAP), y, tile_w, TILE_H};
    }
    return y + TILE_H + SECTION_GAP;
}

void
options_screen_layout(int screen_w, int screen_h, options_screen_layout_t* out) {
    const int w = mathi_min(screen_w - 2 * UI_MARGIN, COLUMN_W_MAX);
    const int x = (screen_w - w) / 2;

    /* The header is the one row a short canvas can spare: every other row is
     * a control, and none may shrink below UI_TAP_MIN. */
    const int header_h = screen_h >= HEADER_H + BODY_H ? HEADER_H : 0;
    out->header = (recti_t){0, 0, screen_w, header_h};
    int y = layout_quality(x, w, header_h + SECTION_GAP, out);
    y = layout_tiles(x, w, y, out);
    out->dither_caption = (recti_t){x, y, w, CAPTION_H};
    out->dither = (recti_t){x, y + CAPTION_H + CAPTION_GAP, w, DROPDOWN_H};

    const int button_w = (w - FOOTER_GAP) / 2;
    const int footer_y = screen_h - BOTTOM_MARGIN - FOOTER_H;
    out->apply = (recti_t){x, footer_y, button_w, FOOTER_H};
    out->cancel = (recti_t){x + w - button_w, footer_y, button_w, FOOTER_H};
}

static void
draw_quality(const options_screen_layout_t* lay, const sand_menu_t* menu, const options_screen_labels_t* labels,
             sand_options_hits_t* hits) {
    ui_panel(lay->quality_panel);
    ui_caption(lay->quality_caption, OPTIONS_SCREEN_QUALITY);
    ui_text_right(lay->quality_value, labels->quality_names[menu->draft.quality]);

    const int count = labels->quality_count;
    int slider = options_screen_slider_from_quality(menu->draft.quality, count);
    if (ui_slider(lay->quality_slider, OPTIONS_SCREEN_QUALITY, &slider, 0, count - 1)) {
        hits->quality = options_screen_quality_from_slider(slider, count);
    }
}

static void
draw_mode_swatch(recti_t tile, const sand_mode_swatch_t* swatch) {
    const recti_t icon = ui_tile_icon_rect(tile);
    const recti_t strip = {tile.x + SWATCH_INSET, icon.y, tile.w - 2 * SWATCH_INSET, icon.h};
    ui_swatches(strip, swatch->rgb, swatch->cols, swatch->rows);
}

static void
draw_colour(const options_screen_layout_t* lay, const sand_menu_t* menu, const options_screen_labels_t* labels,
            sand_options_hits_t* hits) {
    ui_caption_centre(lay->color_caption, OPTIONS_SCREEN_COLOR_MODE);
    for (int i = 0; i < OPTIONS_SCREEN_TILE_COUNT; i++) {
        if (ui_tile(lay->tiles[i], TILE_LABELS[i], menu->draft.color == TILE_COLOURS[i])) {
            hits->color = (int)TILE_COLOURS[i];
        }
        draw_mode_swatch(lay->tiles[i], &labels->mode_swatches[TILE_COLOURS[i]]);
    }
}

static void
draw_dither(const options_screen_layout_t* lay, const sand_menu_t* menu, const options_screen_labels_t* labels,
            sand_options_hits_t* hits) {
    ui_caption_centre(lay->dither_caption, OPTIONS_SCREEN_DITHER);

    assert(labels->dither_count <= GFX_DITHER_MODE_COUNT);
    const draw_image_t* icons[GFX_DITHER_MODE_COUNT];
    const int count = labels->dither_count;
    for (int i = 0; i < count; i++) {
        icons[i] = sand_dither_icon((gfx_dither_mode_t)i);
    }
    int dither = menu->draft.dither;
    if (ui_dropdown_icons(lay->dither, OPTIONS_SCREEN_DITHER, &dither, labels->dither_names, icons, count)) {
        hits->dither = dither;
    }
}

static void
draw_footer(const options_screen_layout_t* lay, const sand_menu_t* menu, sand_options_t committed,
            sand_options_hits_t* hits) {
    const int pending = sand_menu_pending_changes(menu, committed);
    char apply_label[24];
    options_screen_apply_label(pending, apply_label, (int)sizeof apply_label);

    ui_disabled_begin(pending == 0);
    hits->apply = ui_button(lay->apply, apply_label);
    ui_disabled_end();
    hits->cancel = ui_button(lay->cancel, OPTIONS_SCREEN_CANCEL);
}

sand_options_hits_t
options_screen_draw(const sand_menu_t* menu, sand_options_t committed, const options_screen_labels_t* labels) {
    sand_options_hits_t hits = SAND_OPTIONS_NO_HITS;
    ui_set_text_style(UI_TEXT_PLAIN);

    options_screen_layout_t lay;
    options_screen_layout(ui_width(), ui_height(), &lay);

    ui_screen();
    ui_theme(&sand_ui_theme);
    if (lay.header.h > 0) {
        ui_header(lay.header, OPTIONS_SCREEN_TITLE);
    }
    draw_quality(&lay, menu, labels, &hits);
    draw_colour(&lay, menu, labels, &hits);
    draw_footer(&lay, menu, committed, &hits);
    if (sand_menu_dither_applies(menu->draft.color)) {
        draw_dither(&lay, menu, labels, &hits);
    }
    return hits;
}

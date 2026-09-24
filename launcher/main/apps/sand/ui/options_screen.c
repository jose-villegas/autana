#include "options_screen.h"

#include <stdio.h>
#include <string.h>

#include "ui/ui.h"
#include "ui/ui_widgets.h"

#include "apps/sand/icons_dither.h"
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

static const char* const TILE_IDS[OPTIONS_SCREEN_TILE_COUNT] = {"tile 16", "tile 256", "tile full"};

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
min_int(int a, int b) {
    return a < b ? a : b;
}

static int
layout_quality(int x, int w, int y, options_screen_layout_t* out) {
    out->quality_panel = mu_rect(x, y, w, QUALITY_PANEL_H);
    const int inner_x = x + PANEL_PAD;
    const int inner_w = w - 2 * PANEL_PAD;
    const int half = inner_w / 2;
    out->quality_caption = mu_rect(inner_x, y + PANEL_PAD, half, CAPTION_H);
    out->quality_value = mu_rect(inner_x + half, y + PANEL_PAD, inner_w - half, CAPTION_H);
    out->quality_slider = mu_rect(inner_x, y + QUALITY_PANEL_H - PANEL_PAD - SLIDER_H, inner_w, SLIDER_H);
    return y + QUALITY_PANEL_H + SECTION_GAP;
}

static int
layout_tiles(int x, int w, int y, options_screen_layout_t* out) {
    out->color_caption = mu_rect(x, y, w, CAPTION_H);
    y += CAPTION_H + CAPTION_GAP;
    const int n = OPTIONS_SCREEN_TILE_COUNT;
    const int tile_w = (w - (n - 1) * TILE_GAP) / n;
    const int start_x = x + (w - (n * tile_w + (n - 1) * TILE_GAP)) / 2;
    for (int i = 0; i < n; i++) {
        out->tiles[i] = mu_rect(start_x + i * (tile_w + TILE_GAP), y, tile_w, TILE_H);
    }
    return y + TILE_H + SECTION_GAP;
}

void
options_screen_layout(int screen_w, int screen_h, options_screen_layout_t* out) {
    const int w = min_int(screen_w - 2 * UI_MARGIN, COLUMN_W_MAX);
    const int x = (screen_w - w) / 2;

    /* The header is the one row a short canvas can spare: every other row is
     * a control, and none may shrink below UI_TAP_MIN. */
    const int header_h = screen_h >= HEADER_H + BODY_H ? HEADER_H : 0;
    out->header = mu_rect(0, 0, screen_w, header_h);
    int y = layout_quality(x, w, header_h + SECTION_GAP, out);
    y = layout_tiles(x, w, y, out);
    out->dither_caption = mu_rect(x, y, w, CAPTION_H);
    out->dither = mu_rect(x, y + CAPTION_H + CAPTION_GAP, w, DROPDOWN_H);

    const int button_w = (w - FOOTER_GAP) / 2;
    const int footer_y = screen_h - BOTTOM_MARGIN - FOOTER_H;
    out->apply = mu_rect(x, footer_y, button_w, FOOTER_H);
    out->cancel = mu_rect(x + w - button_w, footer_y, button_w, FOOTER_H);
}

static void
draw_quality(mu_Context* ctx, const options_screen_layout_t* lay, const sand_menu_t* menu,
             const options_screen_labels_t* labels, sand_options_hits_t* hits) {
    const ui_theme_t* theme = &sand_ui_theme;
    ui_panel(ctx, lay->quality_panel, theme);
    ui_text_in(ctx, lay->quality_caption, OPTIONS_SCREEN_QUALITY, theme->caption, theme->text_scale, UI_ALIGN_LEFT);
    ui_text_in(ctx, lay->quality_value, labels->quality_names[menu->draft.quality], theme->text, theme->text_scale,
               UI_ALIGN_RIGHT);

    const int count = labels->quality_count;
    int slider = options_screen_slider_from_quality(menu->draft.quality, count);
    mu_layout_set_next(ctx, lay->quality_slider, 0);
    if (ui_slider_int(ctx, &slider, 0, count - 1, 1)) {
        hits->quality = options_screen_quality_from_slider(slider, count);
    }
}

static void
draw_mode_swatch(mu_Context* ctx, mu_Rect tile, const sand_mode_swatch_t* swatch) {
    mu_Color colors[SAND_SWATCH_MAX];
    const int n = swatch->cols * swatch->rows;
    for (int i = 0; i < n; i++) {
        colors[i] = ui_rgb(swatch->rgb[i]);
    }
    const mu_Rect icon = ui_tile_icon_rect(tile, &sand_ui_theme);
    const mu_Rect strip = {tile.x + SWATCH_INSET, icon.y, tile.w - 2 * SWATCH_INSET, icon.h};
    ui_swatch_grid(ctx, strip, colors, swatch->cols, swatch->rows);
}

static void
draw_colour(mu_Context* ctx, const options_screen_layout_t* lay, const sand_menu_t* menu,
            const options_screen_labels_t* labels, sand_options_hits_t* hits) {
    const ui_theme_t* theme = &sand_ui_theme;
    ui_text_in(ctx, lay->color_caption, OPTIONS_SCREEN_COLOR_MODE, theme->caption, theme->text_scale, UI_ALIGN_CENTRE);
    for (int i = 0; i < OPTIONS_SCREEN_TILE_COUNT; i++) {
        const ui_widget_button_t tile = {
            .label = TILE_LABELS[i],
            .enabled = true,
            .selected = menu->draft.color == TILE_COLOURS[i],
        };
        if (ui_tile_button(ctx, TILE_IDS[i], lay->tiles[i], &tile, theme)) {
            hits->color = (int)TILE_COLOURS[i];
        }
        draw_mode_swatch(ctx, lay->tiles[i], &labels->mode_swatches[TILE_COLOURS[i]]);
    }
}

#define DITHER_ITEMS_MAX 8

static void
draw_dither(mu_Context* ctx, const options_screen_layout_t* lay, const sand_menu_t* menu,
            const options_screen_labels_t* labels, sand_options_hits_t* hits) {
    const ui_theme_t* theme = &sand_ui_theme;
    ui_text_in(ctx, lay->dither_caption, OPTIONS_SCREEN_DITHER, theme->caption, theme->text_scale, UI_ALIGN_CENTRE);

    ui_dropdown_item_t items[DITHER_ITEMS_MAX];
    const int count = min_int(labels->dither_count, DITHER_ITEMS_MAX);
    for (int i = 0; i < count; i++) {
        items[i] = (ui_dropdown_item_t){
            .icon = i < ICON_DITHER_COUNT ? &icon_dither_table[i] : NULL,
            .icon_rows = icon_dither_rows,
            .label = labels->dither_names[i],
        };
    }
    const int picked = ui_dropdown(ctx, "dither", lay->dither, items, count, menu->draft.dither, theme);
    if (picked >= 0) {
        hits->dither = picked;
    }
}

static void
draw_footer(mu_Context* ctx, const options_screen_layout_t* lay, const sand_menu_t* menu, sand_options_hits_t* hits) {
    const ui_theme_t* theme = &sand_ui_theme;
    const int pending = sand_menu_pending_changes(menu);
    char apply_label[24];
    options_screen_apply_label(pending, apply_label, (int)sizeof apply_label);

    const ui_widget_button_t apply = {.label = apply_label, .enabled = pending > 0};
    hits->apply = ui_icon_button(ctx, "options apply", lay->apply, &apply, theme);

    const ui_widget_button_t cancel = {.label = OPTIONS_SCREEN_CANCEL, .enabled = true};
    hits->cancel = ui_icon_button(ctx, "options cancel", lay->cancel, &cancel, theme);
}

sand_options_hits_t
options_screen_draw(mu_Context* ctx, const sand_menu_t* menu, const options_screen_labels_t* labels) {
    sand_options_hits_t hits = SAND_OPTIONS_NO_HITS;
    ui_set_text_style(UI_TEXT_PLAIN);

    options_screen_layout_t lay;
    options_screen_layout(ui_width(), ui_height(), &lay);

    if (!ui_begin_screen(ctx, "Sand Options", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        return hits;
    }

    if (lay.header.h > 0) {
        ui_header_bar(ctx, lay.header, OPTIONS_SCREEN_TITLE, sand_ui_theme.text_scale, &sand_ui_theme);
    }
    draw_quality(ctx, &lay, menu, labels, &hits);
    draw_colour(ctx, &lay, menu, labels, &hits);
    draw_footer(ctx, &lay, menu, &hits);
    if (sand_menu_dither_applies(menu->draft.color)) {
        draw_dither(ctx, &lay, menu, labels, &hits);
    }

    mu_end_window(ctx);
    return hits;
}

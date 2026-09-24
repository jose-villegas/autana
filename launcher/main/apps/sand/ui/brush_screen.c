#include "brush_screen.h"

#include <stdio.h>
#include <string.h>

#include "gfx/gfx_color.h"
#include "gfx/gfx_font_roles.h"
#include "ui/ui.h"
#include "ui/ui_style.h"

#include "apps/sand/icons_sand.h"
#include "apps/sand/material_palette.h"
#include "apps/sand/sand_swatch.h"

/* Space between the three stacked panels - and between a panel's own
 * caption row and the control below it, which reuses the same value so the
 * screen reads as one rhythm rather than two unrelated gaps. */
#define BRUSH_SCREEN_GAP   12
#define BRUSH_SCREEN_PAD   8
#define BRUSH_SCREEN_INNER 8

#define CAPTION_ROW_H      20

/* Square, and the header's content height (see HEADER_H below) exactly - the
 * swatch fills the row top to bottom rather than being a smaller square
 * floating inside it. */
#define SWATCH_SIDE        80

#define INFO_BTN_SIDE      UI_TAP_MIN

/* Exactly "06 PX" at scale 2 on the 8px-wide UI font: 5 glyphs * 8px *
 * 2 = 80. Monospace, so this is exact, not a guess - a narrower value
 * clipped the trailing "X" (measured with ui_measure_text() while wiring
 * the screen's drawing code). */
#define SIZE_VALUE_W       80

#define SEG_GAP            8

/* Panel heights, tuned so the whole stack fits the tighter of the two real
 * canvases (448x368 landscape, 336px of content height after margins) with
 * every tap target still >= UI_TAP_MIN - see brush_screen_layout()'s own
 * comment for the sum this must clear. */
#define HEADER_H           96
#define MODE_H             112
#define SIZE_H             100

#define BLOCK_H            (HEADER_H + BRUSH_SCREEN_GAP + MODE_H + BRUSH_SCREEN_GAP + SIZE_H)

/* Both real canvases this screen is ever drawn at - 368x448 portrait and its
 * quarter turn - must have room for BLOCK_H under UI_MARGIN top and bottom;
 * see palette.h's PALETTE_FITS for the same reasoning applied to the other
 * panel in this app. 368 is the tighter height of the two, so it is the one
 * that actually constrains BLOCK_H above. */
_Static_assert(BLOCK_H <= 368 - 2 * UI_MARGIN, "the brush screen's three panels are taller than the "
                                               "shorter of the two real canvases (448x368) allows");

/* Lays `n` equal-width segments across `row`, with `gap` between
 * neighbours, any leftover pixel split evenly on the outside - the same
 * centring shape palette.c's row_left_x() uses, so "equal widths, equal
 * gaps" holds exactly rather than only up to a rounding pixel on one end. */
static void
lay_out_segments(mu_Rect row, int gap, int n, mu_Rect* out) {
    const int seg_w = (row.w - (n - 1) * gap) / n;
    const int used = n * seg_w + (n - 1) * gap;
    const int start_x = row.x + (row.w - used) / 2;

    for (int i = 0; i < n; i++) {
        out[i] = (mu_Rect){start_x + i * (seg_w + gap), row.y, seg_w, row.h};
    }
}

static void
layout_header(mu_Rect panel, brush_screen_layout_t* out) {
    const int content_h = panel.h - 2 * BRUSH_SCREEN_PAD;

    out->swatch = (mu_Rect){panel.x + BRUSH_SCREEN_PAD, panel.y + BRUSH_SCREEN_PAD, SWATCH_SIDE, SWATCH_SIDE};

    out->info_button = (mu_Rect){
        panel.x + panel.w - BRUSH_SCREEN_PAD - INFO_BTN_SIDE,
        panel.y + BRUSH_SCREEN_PAD + (content_h - INFO_BTN_SIDE) / 2,
        INFO_BTN_SIDE,
        INFO_BTN_SIDE,
    };

    const int text_x = out->swatch.x + SWATCH_SIDE + BRUSH_SCREEN_INNER;
    const int text_w = out->info_button.x - BRUSH_SCREEN_INNER - text_x;

    out->material_caption = (mu_Rect){text_x, panel.y + BRUSH_SCREEN_PAD, text_w, CAPTION_ROW_H};
    out->material_name = (mu_Rect){
        text_x,
        out->material_caption.y + CAPTION_ROW_H + 4,
        text_w,
        content_h - CAPTION_ROW_H - 4,
    };
}

static void
layout_mode(mu_Rect panel, brush_screen_layout_t* out) {
    const int content_w = panel.w - 2 * BRUSH_SCREEN_PAD;
    const int content_h = panel.h - 2 * BRUSH_SCREEN_PAD;

    out->mode_caption = (mu_Rect){panel.x + BRUSH_SCREEN_PAD, panel.y + BRUSH_SCREEN_PAD, content_w, CAPTION_ROW_H};

    const mu_Rect row = {
        panel.x + BRUSH_SCREEN_PAD,
        out->mode_caption.y + CAPTION_ROW_H + BRUSH_SCREEN_INNER,
        content_w,
        content_h - CAPTION_ROW_H - BRUSH_SCREEN_INNER,
    };
    lay_out_segments(row, SEG_GAP, BRUSH_SCREEN_SEGMENT_COUNT, out->segments);
}

static void
layout_size(mu_Rect panel, brush_screen_layout_t* out) {
    const int content_w = panel.w - 2 * BRUSH_SCREEN_PAD;
    const int content_h = panel.h - 2 * BRUSH_SCREEN_PAD;

    out->size_value = (mu_Rect){
        panel.x + panel.w - BRUSH_SCREEN_PAD - SIZE_VALUE_W,
        panel.y + BRUSH_SCREEN_PAD,
        SIZE_VALUE_W,
        CAPTION_ROW_H,
    };
    out->size_caption = (mu_Rect){
        panel.x + BRUSH_SCREEN_PAD,
        panel.y + BRUSH_SCREEN_PAD,
        content_w - SIZE_VALUE_W - BRUSH_SCREEN_INNER,
        CAPTION_ROW_H,
    };

    out->slider_track = (mu_Rect){
        panel.x + BRUSH_SCREEN_PAD,
        out->size_caption.y + CAPTION_ROW_H + BRUSH_SCREEN_INNER,
        content_w,
        content_h - CAPTION_ROW_H - BRUSH_SCREEN_INNER,
    };
}

void
brush_screen_layout(int screen_w, int screen_h, brush_screen_layout_t* out) {
    const int panel_x = UI_MARGIN;
    const int panel_w = screen_w - 2 * UI_MARGIN;

    /* Any slack left by BLOCK_H under the taller of the two real canvases
     * (448px, the portrait height) is split evenly above and below rather
     * than left pinned to the top - see BLOCK_H's own _Static_assert. */
    const int top_y = UI_MARGIN + (screen_h - 2 * UI_MARGIN - BLOCK_H) / 2;

    out->header_panel = (mu_Rect){panel_x, top_y, panel_w, HEADER_H};
    out->mode_panel = (mu_Rect){
        panel_x,
        out->header_panel.y + HEADER_H + BRUSH_SCREEN_GAP,
        panel_w,
        MODE_H,
    };
    out->size_panel = (mu_Rect){
        panel_x,
        out->mode_panel.y + MODE_H + BRUSH_SCREEN_GAP,
        panel_w,
        SIZE_H,
    };

    layout_header(out->header_panel, out);
    layout_mode(out->mode_panel, out);
    layout_size(out->size_panel, out);
}

static const char* const SEGMENT_LABELS[BRUSH_SCREEN_SEGMENT_COUNT] = {
    "POUR",
    "ERASE",
    "BOOM",
};

static const char* const SIZE_CAPTIONS[BRUSH_SCREEN_SEGMENT_COUNT] = {
    "POUR SIZE",
    "ERASE SIZE",
    "BOOM SIZE",
};

const char*
brush_screen_segment_label(brush_screen_segment_t seg) {
    return SEGMENT_LABELS[seg];
}

const char*
brush_screen_size_caption(brush_screen_segment_t seg) {
    return SIZE_CAPTIONS[seg];
}

#define BRUSH_INFO_ICON_PAD 12

static const icon_t* const brush_seg_icons[BRUSH_SCREEN_SEGMENT_COUNT] = {
    [BRUSH_SCREEN_SEG_POUR] = &icon_sand_table[ICON_SAND_POUR],
    [BRUSH_SCREEN_SEG_ERASE] = &icon_sand_table[ICON_SAND_ERASE],
    [BRUSH_SCREEN_SEG_BOOM] = &icon_sand_table[ICON_SAND_BOOM],
};

static void
draw_brush_bezel(mu_Context* ctx, mu_Rect r, uint32_t face_rgb, bool sunken) {
    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n = ui_bezel_spans(r, ui_rgb(face_rgb), sunken, spans, UI_BEZEL_MAX_SPANS);
    for (int i = 0; i < n; i++) {
        mu_draw_rect(ctx, spans[i].rect, spans[i].color);
    }
}

#define BRUSH_SWATCH_CELLS 8

static void
draw_brush_swatch(mu_Context* ctx, mu_Rect r, cell_t spec) {
    const gfx_color_t* palette = material_palette();
    mu_Color colors[BRUSH_SWATCH_CELLS * BRUSH_SWATCH_CELLS];
    for (int row = 0; row < BRUSH_SWATCH_CELLS; row++) {
        for (int col = 0; col < BRUSH_SWATCH_CELLS; col++) {
            const cell_t cell = sand_swatch_cell(spec, col, row, BRUSH_SWATCH_CELLS);
            colors[row * BRUSH_SWATCH_CELLS + col] = ui_rgb(gfx_color_rgb888(palette[cell]));
        }
    }
    ui_swatch_grid(ctx, r, colors, BRUSH_SWATCH_CELLS, BRUSH_SWATCH_CELLS);

    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n =
        ui_bezel_spans(r, ui_rgb(gfx_color_rgb888(material_brush_color(spec))), false, spans, UI_BEZEL_MAX_SPANS);
    for (int i = 1; i < n; i++) {
        mu_draw_rect(ctx, spans[i].rect, spans[i].color);
    }
}

static void
draw_brush_header(mu_Context* ctx, sand_ui_t* ui, const brush_screen_layout_t* lay) {
    ui_set_font_scaled(gfx_font_ui(), BRUSH_SCREEN_CAPTION_SCALE);

    ui_panel(ctx, lay->header_panel, &sand_ui_theme);
    draw_brush_swatch(ctx, lay->swatch, ui->brushes[ui->brush].cell);
    ui_text_in(ctx, lay->material_caption, BRUSH_SCREEN_MATERIAL_CAPTION, sand_ui_theme.caption,
               BRUSH_SCREEN_CAPTION_SCALE, UI_ALIGN_LEFT);

    /* 4 is the starting scale, but "Gunpowder" (the longest name any
     * brush carries) doesn't fit it in the name rect at the narrower
     * of the two real canvases - drop a size at a time rather than let
     * ui_text_in()'s clip cut the tail off a real material name. */
    const char* name = material_name(ui->brushes[ui->brush].cell);
    int name_scale = 4;
    for (; name_scale > 1; name_scale--) {
        ui_set_font_scaled(gfx_font_ui(), name_scale);
        if (ui_measure_text(name) <= lay->material_name.w) {
            break;
        }
    }
    ui_text_in(ctx, lay->material_name, name, sand_ui_theme.text, name_scale, UI_ALIGN_LEFT);
    ui_set_font_scaled(gfx_font_ui(), BRUSH_SCREEN_CAPTION_SCALE);

    draw_brush_bezel(ctx, lay->info_button, SAND_THEME_BUTTON_FACE_COLOR, false);
    /* No handler: the panel this button opens is separate, later work.
     * Drawn now because it's in the design; not a bug that tapping it
     * does nothing yet. */
    const mu_Rect icon_r = {
        lay->info_button.x + BRUSH_INFO_ICON_PAD,
        lay->info_button.y + BRUSH_INFO_ICON_PAD,
        lay->info_button.w - 2 * BRUSH_INFO_ICON_PAD,
        lay->info_button.h - 2 * BRUSH_INFO_ICON_PAD,
    };
    ui_draw_icon(ctx, icon_r, &icon_sand_table[ICON_SAND_INFO], icon_sand_rows, sand_ui_theme.text);
}

static void
draw_brush_mode_segment(mu_Context* ctx, sand_ui_t* ui, mu_Rect r, int i) {
    const char* seg_name = brush_screen_segment_label((brush_screen_segment_t)i);
    const ui_widget_button_t segment = {
        .icon = brush_seg_icons[i],
        .icon_rows = icon_sand_rows,
        .label = seg_name,
        .enabled = true,
        .selected = (sand_mode_t)i == ui->mode,
    };
    if (ui_tile_button(ctx, seg_name, r, &segment, &sand_ui_theme)) {
        sand_ui_mode_clicked(ui, i);
    }
}

static void
draw_brush_mode_block(mu_Context* ctx, sand_ui_t* ui, const brush_screen_layout_t* lay) {
    ui_panel(ctx, lay->mode_panel, &sand_ui_theme);
    ui_text_in(ctx, lay->mode_caption, BRUSH_SCREEN_MODE_CAPTION, sand_ui_theme.caption, BRUSH_SCREEN_CAPTION_SCALE,
               UI_ALIGN_LEFT);

    for (int i = 0; i < BRUSH_SCREEN_SEGMENT_COUNT; i++) {
        draw_brush_mode_segment(ctx, ui, lay->segments[i], i);
    }
}

static void
draw_brush_size_block(mu_Context* ctx, sand_ui_t* ui, const brush_screen_layout_t* lay) {
    ui_panel(ctx, lay->size_panel, &sand_ui_theme);

    const char* size_caption = brush_screen_size_caption((brush_screen_segment_t)ui->mode);
    ui_text_in(ctx, lay->size_caption, size_caption, sand_ui_theme.caption, BRUSH_SCREEN_CAPTION_SCALE, UI_ALIGN_LEFT);

    char size_value[8];
    snprintf(size_value, sizeof size_value, "%02u PX", (unsigned)sand_ui_radius(ui));
    ui_text_in(ctx, lay->size_value, size_value, sand_ui_theme.text, BRUSH_SCREEN_CAPTION_SCALE, UI_ALIGN_RIGHT);

    int radius = sand_ui_radius(ui);
    if (ui_theme_slider_int(ctx, lay->slider_track, &radius, SAND_UI_RADIUS_MIN, SAND_UI_RADIUS_MAX, 1,
                            &sand_ui_theme)) {
        sand_ui_set_radius(ui, (uint8_t)radius);
    }
}

void
brush_screen_draw(mu_Context* ctx, sand_ui_t* ui) {
    ui_set_text_style(UI_TEXT_PLAIN);

    brush_screen_layout_t lay;
    brush_screen_layout(ui_width(), ui_height(), &lay);

    if (!ui_begin_screen(ctx, "Sand Brush", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        return;
    }

    draw_brush_header(ctx, ui, &lay);
    draw_brush_mode_block(ctx, ui, &lay);
    draw_brush_size_block(ctx, ui, &lay);

    mu_end_window(ctx);
}

/*
 * brush_screen_render_host - the sand app's brush screen drawn on a host, at
 * both canvas sizes, so its composition can be judged without a device build
 * or a flash. Real gfx and layout code, a malloc'd framebuffer, no device;
 * the harness in launcher/tools/render/render_host.h owns the frame loop and
 * the BMP.
 *
 * EVERY RECT COMES FROM brush_screen_layout() - nothing here hardcodes a
 * position. A preview that disagrees with the screen it previews is worse
 * than no preview.
 *
 * Approximated, since this stays out of ui.c and microui.c: nothing is
 * pressed or focused; the slider draws in microui's default colours (so does
 * the real screen, making that a value to know rather than a gap); a
 * segment's icon/label sub-layout is mirrored from draw_brush_screen()'s own
 * inline arithmetic; and a flat fill stands in for the frozen sandbox.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/draw/gfx_color.h"
#include "gfx/draw/gfx_draw.h"
#include "gfx/draw/gfx_font_roles.h"
#include "gfx/draw/icon.h"
#include "gfx/gfx.h"
#include "material.h"
#include "material_palette.h"
#include "render_host.h"
#include "sand_brushes.h"
#include "sand_swatch.h"
#include "sand_ui.h"
#include "ui/brush_screen.h"
#include "ui/sand_icons.h"
#include "ui/ui.h" /* UI_MARGIN, UI_SLIDER_KNOB_W only */
#include "ui/ui_slider.h"
#include "ui/ui_style.h"
#include "ui/ui_transform.h"

/* A real brush cell, mode and radius to draw: the longest name, the worst
 * case brush_screen.c's name-shrink loop handles. */
#define PREVIEW_MATERIAL    (sand_brushes[sand_brush_longest_name()].cell)
#define PREVIEW_MODE        SAND_MODE_PAINT
#define PREVIEW_RADIUS_PX   ((SAND_UI_RADIUS_MIN + SAND_UI_RADIUS_MAX) / 2)

/* app_sand.c's own inline sub-layout inside draw_brush_screen(), see this
 * file's header for why these are mirrored rather than shared. */
#define SEG_PAD             8
#define SEG_LABEL_GAP       4
#define INFO_ICON_PAD       12
#define SWATCH_CELLS        8

/* microui's default_style (components/microui/src/microui.c), see this
 * file's header for why the slider has to carry these rather than ask
 * ui.c for them. */
#define SLIDER_BASE_COLOR   0x1E1E1E /* MU_COLOR_BASE        30,30,30 */
#define SLIDER_BORDER_COLOR 0x191919 /* MU_COLOR_BORDER      25,25,25 */
#define SLIDER_FILL_COLOR   0x737373 /* MU_COLOR_BUTTONFOCUS 115,115,115 */
#define SLIDER_KNOB_COLOR   0x4B4B4B /* MU_COLOR_BUTTON      75,75,75 */

static mu_Color
hex_to_mu(uint32_t rgb) {
    return (mu_Color){(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255};
}

static gfx_color_t
mu_to_gfx(mu_Color c) {
    return gfx_rgb(((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b);
}

static void
fill_rect_logical(ui_transform_t t, mu_Rect r, gfx_color_t color) {
    const mu_Rect p = ui_transform_rect(t, r);
    gfx_fill_rect(p.x, p.y, p.w, p.h, color);
}

/* Mirrors ui_panel(): one flat section frame. */
static void
draw_panel(ui_transform_t t, mu_Rect r, uint32_t face_rgb, uint32_t border_rgb) {
    ui_span_t spans[UI_PANEL_MAX_SPANS];
    const int n = ui_panel_spans(r, hex_to_mu(face_rgb), hex_to_mu(border_rgb), spans, UI_PANEL_MAX_SPANS);
    for (int i = 0; i < n; i++) {
        fill_rect_logical(t, spans[i].rect, mu_to_gfx(spans[i].color));
    }
}

/* Mirrors brush_screen.c's draw_brush_bezel(); `sunken` is always false here -
 * see this file's header on hit state. */
static void
draw_bezel(ui_transform_t t, mu_Rect r, uint32_t face_rgb) {
    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n = ui_bezel_spans(r, hex_to_mu(face_rgb), false, spans, UI_BEZEL_MAX_SPANS);
    for (int i = 0; i < n; i++) {
        fill_rect_logical(t, spans[i].rect, mu_to_gfx(spans[i].color));
    }
}

typedef struct {
    gfx_color_t color;
} icon_fill_ctx_t;

static void
icon_emit(void* ctx, int x, int y, int w, int h) {
    gfx_fill_rect(x, y, w, h, ((const icon_fill_ctx_t*)ctx)->color);
}

/* Mirrors ui.c's ui_draw_icon(), with `box` mapped through `t` the way
 * ui.c's own draw_command() maps MU_COMMAND_ICON; no icon draws nothing. */
static void
draw_icon(ui_transform_t t, mu_Rect box, const gfx_image_t* icon, uint32_t color_rgb) {
    if (icon != NULL) {
        icon_fill_ctx_t ctx = {gfx_rgb(color_rgb)};
        ui_transform_icon_blocks(t, icon, box, icon_emit, &ctx);
    }
}

/* Mirrors ui_text_in() for the logical (x, y), then ui.c's
 * draw_command() MU_COMMAND_TEXT case for turning it into a physical glyph
 * origin, see gfx_text_font() and ui_text_glyph0_origin(). The brush
 * screen always draws UI_TEXT_PLAIN, so this is the ink pass alone, no
 * halo. */
static void
draw_text(ui_transform_t t, mu_Rect r, const char* str, uint32_t color_rgb, int scale, int align) {
    const gfx_font_t* font = gfx_font_ui();
    const int tw = gfx_font_text_width(font, str, -1, scale);
    const int th = gfx_font_height(font, scale);
    const int x = (align < 0) ? r.x : (align == 0) ? r.x + (r.w - tw) / 2 : r.x + r.w - tw;
    const int y = r.y + (r.h - th) / 2;

    const int quarter = ui_transform_quarter(t);
    const mu_Rect box = ui_transform_rect(t, (mu_Rect){x, y, tw, th});
    int mx, my;
    ui_text_glyph0_origin(font, box, quarter, scale, &mx, &my);
    gfx_text_font(mx, my, str, gfx_rgb(color_rgb), scale, quarter, font);
}

/* Mirrors ui/brush_screen.c's draw_brush_swatch(). */
static void
draw_swatch(ui_transform_t t, mu_Rect r, cell_t spec) {
    const gfx_color_t* palette = material_palette();
    for (int row = 0; row < SWATCH_CELLS; row++) {
        const int y0 = r.y + row * r.h / SWATCH_CELLS;
        const int y1 = r.y + (row + 1) * r.h / SWATCH_CELLS;
        for (int col = 0; col < SWATCH_CELLS; col++) {
            const int x0 = r.x + col * r.w / SWATCH_CELLS;
            const int x1 = r.x + (col + 1) * r.w / SWATCH_CELLS;
            const cell_t cell = sand_swatch_cell(spec, col, row, SWATCH_CELLS);
            fill_rect_logical(t, (mu_Rect){x0, y0, x1 - x0, y1 - y0}, palette[cell]);
        }
    }

    ui_span_t spans[UI_BEZEL_MAX_SPANS];
    const int n =
        ui_bezel_spans(r, hex_to_mu(gfx_color_rgb888(material_brush_color(spec))), false, spans, UI_BEZEL_MAX_SPANS);
    for (int i = 1; i < n; i++) {
        fill_rect_logical(t, spans[i].rect, mu_to_gfx(spans[i].color));
    }
}

/* Mirrors ui.c's ui_slider_int() body, minus hit-testing (nothing is being
 * dragged in a static preview) - same span order (face, fill, border,
 * knob), see that function's own comment for why. */
static void
draw_slider(ui_transform_t t, mu_Rect track, int lo, int hi, int value) {
    ui_span_t panel[UI_PANEL_MAX_SPANS];
    const int pn =
        ui_panel_spans(track, hex_to_mu(SLIDER_BASE_COLOR), hex_to_mu(SLIDER_BORDER_COLOR), panel, UI_PANEL_MAX_SPANS);
    if (pn <= 0) {
        return;
    }
    fill_rect_logical(t, panel[0].rect, mu_to_gfx(panel[0].color));
    fill_rect_logical(t, ui_slider_fill_rect(track, lo, hi, value, UI_SLIDER_KNOB_W), gfx_rgb(SLIDER_FILL_COLOR));
    for (int i = 1; i < pn; i++) {
        fill_rect_logical(t, panel[i].rect, mu_to_gfx(panel[i].color));
    }

    ui_span_t knob[UI_BEZEL_MAX_SPANS];
    const mu_Rect knob_rect = ui_slider_knob_rect(track, lo, hi, value, UI_SLIDER_KNOB_W);
    const int kn = ui_bezel_spans(knob_rect, hex_to_mu(SLIDER_KNOB_COLOR), false, knob, UI_BEZEL_MAX_SPANS);
    for (int i = 0; i < kn; i++) {
        fill_rect_logical(t, knob[i].rect, mu_to_gfx(knob[i].color));
    }
}

/* Everything draw_brush_screen() (app_sand.c) draws, minus the hit-testing
 * and the microui plumbing around it, see this file's header for exactly
 * what that leaves out. Every rect comes from `lay`, filled by
 * brush_screen_layout() itself - see this file's header, Task C. */
static void
draw_screen(ui_transform_t t, int screen_w, int screen_h) {
    brush_screen_layout_t lay;
    brush_screen_layout(screen_w, screen_h, &lay);

    const gfx_font_t* font = gfx_font_ui();
    const int scale = BRUSH_SCREEN_CAPTION_SCALE;

    /* Header: swatch, caption/name, info button. */
    draw_panel(t, lay.header_panel, SAND_THEME_PANEL_FACE_COLOR, SAND_THEME_PANEL_EDGE_COLOR);
    draw_swatch(t, lay.swatch, PREVIEW_MATERIAL);
    draw_text(t, lay.material_caption, BRUSH_SCREEN_MATERIAL_CAPTION, SAND_THEME_CAPTION_COLOR, scale, -1);

    const char* name = material_name(PREVIEW_MATERIAL);
    int name_scale = 4;
    for (; name_scale > 1; name_scale--) {
        if (gfx_font_text_width(font, name, -1, name_scale) <= lay.material_name.w) {
            break;
        }
    }
    draw_text(t, lay.material_name, name, SAND_THEME_TEXT_COLOR, name_scale, -1);

    draw_bezel(t, lay.info_button, SAND_THEME_BUTTON_FACE_COLOR);
    {
        const mu_Rect icon_r = {
            lay.info_button.x + INFO_ICON_PAD,
            lay.info_button.y + INFO_ICON_PAD,
            lay.info_button.w - 2 * INFO_ICON_PAD,
            lay.info_button.h - 2 * INFO_ICON_PAD,
        };
        if (sand_icon(SAND_ICON_INFO) != NULL) {
            draw_icon(t, icon_r, sand_icon(SAND_ICON_INFO), SAND_THEME_TEXT_COLOR);
        } else {
            draw_text(t, icon_r, sand_icon_set.names[SAND_ICON_INFO].text, SAND_THEME_TEXT_COLOR, scale, 0);
        }
    }

    /* Brush mode: caption, three segments. */
    draw_panel(t, lay.mode_panel, SAND_THEME_PANEL_FACE_COLOR, SAND_THEME_PANEL_EDGE_COLOR);
    draw_text(t, lay.mode_caption, BRUSH_SCREEN_MODE_CAPTION, SAND_THEME_CAPTION_COLOR, scale, -1);

    for (int i = 0; i < BRUSH_SCREEN_SEGMENT_COUNT; i++) {
        const mu_Rect r = lay.segments[i];
        const char* label = brush_screen_segment_label((brush_screen_segment_t)i);
        const bool selected = ((sand_mode_t)i == PREVIEW_MODE);
        const uint32_t face = selected ? SAND_THEME_SELECTED_COLOR : SAND_THEME_BUTTON_FACE_COLOR;
        const uint32_t ink = selected ? SAND_THEME_ON_SELECTED_COLOR : SAND_THEME_TEXT_COLOR;

        draw_bezel(t, r, face);

        const int label_h = gfx_font_height(font, scale);
        int icon_side = r.h - 2 * SEG_PAD - label_h - SEG_LABEL_GAP;
        const int icon_side_max = r.w - 2 * SEG_PAD;
        if (icon_side > icon_side_max) {
            icon_side = icon_side_max;
        }
        const mu_Rect icon_r = {
            r.x + (r.w - icon_side) / 2,
            r.y + SEG_PAD,
            icon_side,
            icon_side,
        };
        draw_icon(t, icon_r, sand_icon((sand_icon_id_t)i), ink);

        const mu_Rect label_r = {
            r.x + SEG_PAD,
            icon_r.y + icon_side + SEG_LABEL_GAP,
            r.w - 2 * SEG_PAD,
            label_h,
        };
        draw_text(t, label_r, label, ink, scale, 0);
    }

    /* Brush size: caption/value, slider. */
    draw_panel(t, lay.size_panel, SAND_THEME_PANEL_FACE_COLOR, SAND_THEME_PANEL_EDGE_COLOR);
    draw_text(t, lay.size_caption, brush_screen_size_caption((brush_screen_segment_t)PREVIEW_MODE),
              SAND_THEME_CAPTION_COLOR, scale, -1);

    char size_value[8];
    snprintf(size_value, sizeof size_value, "%02u PX", (unsigned)PREVIEW_RADIUS_PX);
    draw_text(t, lay.size_value, size_value, SAND_THEME_TEXT_COLOR, scale, 1);

    draw_slider(t, lay.slider_track, SAND_UI_RADIUS_MIN, SAND_UI_RADIUS_MAX, PREVIEW_RADIUS_PX);
}

static ui_transform_t transform;
static int screen_w;
static int screen_h;

int
display_quarter_now(void) {
    return 0;
}

static bool
setup(int quarter) {
    transform = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT);
    (void)ui_icon_set_load(&sand_icon_set);
    screen_w = (quarter % 2 == 0) ? GFX_WIDTH : GFX_HEIGHT;
    screen_h = (quarter % 2 == 0) ? GFX_HEIGHT : GFX_WIDTH;
    return true;
}

static void
draw(const render_frame_t* frame) {
    (void)frame;
    gfx_clear(gfx_rgb(0x000000));
    draw_screen(transform, screen_w, screen_h);
}

const render_scene_t render_scene = {
    .name = "brush_screen",
    .quarter = 0,
    .frames = 3,
    .dt_ms = 16,
    .setup = setup,
    .draw = draw,
};

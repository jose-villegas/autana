#include "boot/post_layout.h"

#include "display/display.h"

/* Leading between report lines, the air under the title and under the
 * summary line, and the gutter between columns. */
#define LINE_GAP    2
#define TITLE_GAP   6
#define SUMMARY_GAP 8
#define COLUMN_GAP  8

static mu_Rect
empty_rect(void) {
    const mu_Rect r = {0, 0, 0, 0};
    return r;
}

static int
column_width(int band_w, int columns) {
    return (band_w - (columns - 1) * COLUMN_GAP) / columns;
}

static mu_Rect
band(mu_Rect safe, int offset_y, int h) {
    return ui_anchor_rect(safe, UI_ANCHOR_TOP, UI_ANCHOR_TOP, 0, offset_y, safe.w, h);
}

static mu_Rect
centred_text(mu_Rect in, int text_w) {
    return ui_anchor_rect(in, UI_ANCHOR_CENTER, UI_ANCHOR_CENTER, 0, 0, text_w, in.h);
}

post_layout_t
post_layout(const gfx_font_t* font, int screen_w, int screen_h) {
    post_layout_t l = {0};

    const int inset = DISPLAY_PANEL_SAFE_INSET;
    const mu_Rect screen = {0, 0, screen_w, screen_h};
    l.safe = ui_rect_inset(screen, inset, inset, inset, inset);
    l.glyph_w = gfx_font_text_width(font, " ", 1, POST_LAYOUT_REPORT_SCALE);
    l.line_h = gfx_font_height(font, POST_LAYOUT_REPORT_SCALE) + LINE_GAP;
    l.column_gap = COLUMN_GAP;

    const int title_h = gfx_font_height(font, POST_LAYOUT_TITLE_SCALE);
    l.title = band(l.safe, 0, title_h);
    l.summary = band(l.safe, title_h + TITLE_GAP, l.line_h);
    l.footer = ui_anchor_rect(l.safe, UI_ANCHOR_BOTTOM, UI_ANCHOR_BOTTOM, 0, 0, l.safe.w, l.line_h);

    l.body = ui_rect_inset(l.safe, 0, title_h + TITLE_GAP + l.line_h + SUMMARY_GAP, 0, l.line_h + SUMMARY_GAP);
    if (l.body.w <= 0 || l.body.h <= 0 || l.glyph_w <= 0) {
        return l;
    }

    /* The orientation asks for a column count and the canvas vetoes it: a
     * column too narrow for the widest line costs a column rather than
     * truncating every line in it. */
    l.columns = screen_w > screen_h ? POST_LAYOUT_LANDSCAPE_COLUMNS : POST_LAYOUT_PORTRAIT_COLUMNS;
    const int widest = gfx_font_text_width(font, POST_LAYOUT_WIDEST_LINE, -1, POST_LAYOUT_REPORT_SCALE);
    while (l.columns > 1 && column_width(l.body.w, l.columns) < widest) {
        l.columns--;
    }

    l.column_w = column_width(l.body.w, l.columns);
    l.line_chars = l.column_w / l.glyph_w;
    l.rows = l.body.h / l.line_h;
    return l;
}

mu_Rect
post_layout_title_text(const post_layout_t* l, int text_w) {
    return centred_text(l->title, text_w);
}

mu_Rect
post_layout_summary_text(const post_layout_t* l, int text_w) {
    return centred_text(l->summary, text_w);
}

mu_Rect
post_layout_footer_text(const post_layout_t* l, int text_w) {
    return centred_text(l->footer, text_w);
}

mu_Rect
post_layout_column(const post_layout_t* l, int column) {
    if (column < 0 || column >= l->columns) {
        return empty_rect();
    }
    return ui_anchor_rect(l->body, UI_ANCHOR_TOP_LEFT, UI_ANCHOR_TOP_LEFT, column * (l->column_w + l->column_gap), 0,
                          l->column_w, l->rows * l->line_h);
}

mu_Rect
post_layout_line(const post_layout_t* l, int index) {
    if (index < 0 || index >= post_layout_capacity(l)) {
        return empty_rect();
    }

    const mu_Rect column = post_layout_column(l, index / l->rows);
    return ui_anchor_rect(column, UI_ANCHOR_TOP_LEFT, UI_ANCHOR_TOP_LEFT, 0, (index % l->rows) * l->line_h, column.w,
                          l->line_h);
}

int
post_layout_capacity(const post_layout_t* l) {
    return l->columns * l->rows;
}

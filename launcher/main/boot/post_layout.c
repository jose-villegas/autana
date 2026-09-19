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

mu_Rect
post_layout_take_line(const post_layout_t* l, post_lines_t* lines) {
    const mu_Rect line = post_layout_line(l, lines->next);
    if (line.w > 0) {
        lines->next++;
    }
    return line;
}

/* A column break already reads as a break, so a gap that would land on a
 * column's first row is dropped rather than drawn: spending the row would
 * start that column one line below every other one. */
void
post_layout_gap(const post_layout_t* l, post_lines_t* lines) {
    if (l->rows <= 0 || lines->next % l->rows == 0) {
        return;
    }
    lines->next++;
}

void
post_layout_reserve(const post_layout_t* l, post_lines_t* lines, int n) {
    if (l->rows <= 0 || n <= 0) {
        return;
    }

    const int row = lines->next % l->rows;
    if (row == 0 || n <= l->rows - row) {
        return;
    }

    /* Past the last column there is nowhere better to go, and skipping the
     * rows that are left would cost the report more than the break saves. */
    const int next_top = lines->next + (l->rows - row);
    if (next_top >= post_layout_capacity(l)) {
        return;
    }
    lines->next = next_top;
}

int
post_layout_wrap_columns(const post_layout_t* l, int indent) {
    int columns = l->line_chars - indent;
    if (columns > POST_WRAP_MAX_CHARS) {
        columns = POST_WRAP_MAX_CHARS;
    }
    return columns < 1 ? 1 : columns;
}

post_wrap_line_t
post_wrap_next(const char* text, int columns, int* cursor) {
    if (columns < 1) {
        columns = 1;
    }

    int at = *cursor;
    while (text[at] == ' ') {
        at++; /* the break the previous line consumed */
    }

    post_wrap_line_t line = {at, 0};
    if (text[at] == '\0') {
        *cursor = at;
        return line;
    }

    while (text[at + line.len] != '\0' && line.len < columns) {
        line.len++;
    }
    if (text[at + line.len] != '\0') {
        int brk = columns;
        while (brk > 0 && text[at + brk] != ' ') {
            brk--;
        }
        line.len = brk > 0 ? brk : columns;
    }

    *cursor = at + line.len;
    return line;
}

int
post_wrap_count(const char* text, int columns) {
    int cursor = 0;
    int lines = 0;
    while (post_wrap_next(text, columns, &cursor).len > 0) {
        lines++;
    }
    return lines;
}

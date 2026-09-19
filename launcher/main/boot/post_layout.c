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

int
post_layout_max_columns(int screen_w, int screen_h) {
    return screen_w > screen_h ? POST_LAYOUT_LANDSCAPE_COLUMNS : POST_LAYOUT_PORTRAIT_COLUMNS;
}

post_layout_t
post_layout_with(const gfx_font_t* font, int screen_w, int screen_h, int columns, int gap, bool has_footer) {
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
    /* A band with nothing in it takes no height: the normal report carries
     * no footer, and reserving its line there costs the columns rows the
     * report has to truncate to pay for. */
    const int footer_h = has_footer ? l.line_h : 0;
    const int footer_band = has_footer ? footer_h + SUMMARY_GAP : 0;
    l.footer = ui_anchor_rect(l.safe, UI_ANCHOR_BOTTOM, UI_ANCHOR_BOTTOM, 0, 0, l.safe.w, footer_h);

    l.body = ui_rect_inset(l.safe, 0, title_h + TITLE_GAP + l.line_h + SUMMARY_GAP, 0, footer_band);
    if (l.body.w <= 0 || l.body.h <= 0 || l.glyph_w <= 0) {
        return l;
    }

    /* The caller asks for a column count and the canvas vetoes it: a column
     * too narrow for the widest line costs a column rather than truncating
     * every line in it. */
    l.columns = columns < 1 ? 1 : columns;
    const int widest = gfx_font_text_width(font, POST_LAYOUT_WIDEST_LINE, -1, POST_LAYOUT_REPORT_SCALE);
    while (l.columns > 1 && column_width(l.body.w, l.columns) < widest) {
        l.columns--;
    }

    l.column_w = column_width(l.body.w, l.columns);
    l.line_chars = l.column_w / l.glyph_w;
    l.rows = l.body.h / l.line_h;
    l.gap = gap < 0 ? 0 : gap;
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

int
post_layout_capacity_px(const post_layout_t* l) {
    return l->columns * l->rows * l->line_h;
}

/* Pixels of this column still able to hold a line. */
static int
room_left(const post_layout_t* l, const post_lines_t* lines) {
    return l->rows * l->line_h - lines->y;
}

static bool
step_to_next_column(const post_layout_t* l, post_lines_t* lines) {
    if (lines->column + 1 >= l->columns) {
        return false;
    }
    lines->column++;
    lines->y = 0;
    return true;
}

mu_Rect
post_layout_take_line(const post_layout_t* l, post_lines_t* lines) {
    if (l->line_h <= 0 || l->columns <= 0 || lines->column >= l->columns) {
        return empty_rect();
    }
    if (room_left(l, lines) < l->line_h && !step_to_next_column(l, lines)) {
        return empty_rect();
    }

    const mu_Rect column = post_layout_column(l, lines->column);
    const mu_Rect line = {column.x, l->body.y + lines->y, column.w, l->line_h};
    lines->y += l->line_h;
    return line;
}

/* A column break already reads as a break, so air that would land at the top
 * of a column is dropped rather than drawn: spending it there would start
 * that column below every other one. */
void
post_layout_gap(const post_layout_t* l, post_lines_t* lines) {
    if (l->gap <= 0 || lines->y == 0) {
        return;
    }
    lines->y += l->gap;
}

void
post_layout_reserve(const post_layout_t* l, post_lines_t* lines, int n) {
    if (l->line_h <= 0 || n <= 0 || lines->y == 0) {
        return;
    }
    if (n * l->line_h <= room_left(l, lines)) {
        return;
    }

    /* Past the last column there is nowhere better to go, and skipping what
     * is left would cost the report more than the break saves. */
    step_to_next_column(l, lines);
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

int
post_layout_entry_height(const post_layout_t* l, const char* detail) {
    if (detail == NULL || detail[0] == '\0') {
        return 1;
    }
    return 1 + post_wrap_count(detail, post_layout_wrap_columns(l, POST_LAYOUT_MARK_CHARS));
}

/* The pass the drawer would walk, walked rather than drawn - so what "fits"
 * means here cannot drift from where the drawer actually stops. */
static bool
report_fits(const post_layout_t* l, const post_entries_t* entries) {
    if (post_layout_capacity(l) <= 0) {
        return false;
    }

    post_lines_t lines = {0, 0};
    for (int i = 0; i < entries->count; i++) {
        const int height = post_layout_entry_height(l, entries->detail(entries->ctx, i));
        post_layout_reserve(l, &lines, height);
        for (int line = 0; line < height; line++) {
            if (post_layout_take_line(l, &lines).w <= 0) {
                return false;
            }
        }
        post_layout_gap(l, &lines);
    }
    return true;
}

static bool
fits_at(const gfx_font_t* font, int screen_w, int screen_h, int columns, int gap, const post_entries_t* entries,
        post_layout_t* out) {
    const post_layout_t l = post_layout_with(font, screen_w, screen_h, columns, gap, entries->has_footer);
    if (!report_fits(&l, entries)) {
        return false;
    }
    *out = l;
    return true;
}

static bool
fewest_columns_that_fit(const gfx_font_t* font, int screen_w, int screen_h, int ceiling, int gap,
                        const post_entries_t* entries, post_layout_t* out) {
    for (int columns = 1; columns <= ceiling; columns++) {
        if (fits_at(font, screen_w, screen_h, columns, gap, entries, out)) {
            return true;
        }
    }
    return false;
}

/* Every column, and the most air a uniform gap can carry there. Walked down
 * from a full line rather than solved: the gap that fits depends on which
 * checks land in which column, which is the placement's answer, not
 * arithmetic on the totals. */
static bool
widest_gap_that_fits(const gfx_font_t* font, int screen_w, int screen_h, int ceiling, int full_gap,
                     const post_entries_t* entries, post_layout_t* out) {
    for (int gap = full_gap - 1; gap > 0; gap--) {
        if (fits_at(font, screen_w, screen_h, ceiling, gap, entries, out)) {
            return true;
        }
    }
    return false;
}

post_layout_t
post_layout_for_report(const gfx_font_t* font, int screen_w, int screen_h, const post_entries_t* entries) {
    const int ceiling = post_layout_max_columns(screen_w, screen_h);
    const post_layout_t plain = post_layout_with(font, screen_w, screen_h, ceiling, 0, entries->has_footer);

    post_layout_t chosen;
    if (fewest_columns_that_fit(font, screen_w, screen_h, ceiling, plain.line_h, entries, &chosen)) {
        return chosen;
    }
    if (widest_gap_that_fits(font, screen_w, screen_h, ceiling, plain.line_h, entries, &chosen)) {
        return chosen;
    }
    return plain;
}

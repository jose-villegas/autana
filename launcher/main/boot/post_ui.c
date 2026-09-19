/*
 * post_ui - drawing the self-test report on the panel.
 *
 * Kept out of post.c so that file stays about hardware, and out of
 * post_layout.c so the geometry stays answerable on a host. Shared by the
 * failure screen at boot and by whichever app re-runs the checks.
 */

#include "boot/post_ui.h"

#include <stdio.h>
#include <string.h>

#include "boot/post_layout.h"
#include "gfx/gfx.h"
#include "ui/ui_transform.h"

#define BG_RGB      0x0A0C14
#define HEADING_RGB 0xE6EAF2
#define OK_RGB      0x3DDC97
#define FAIL_RGB    0xFF5C5C
#define ABSENT_RGB  0x6E778C
#define DETAIL_RGB  0x8A93A8

/* The status mark and the space after it: the name and every wrapped detail
 * line begin that far into the column. */
#define MARK_CHARS  5

/* One pass over the results, holding everything that does not change between
 * lines: where the next line goes, and how to get a logical rect onto the
 * panel. */
typedef struct {
    post_layout_t layout;
    post_lines_t lines;
    ui_transform_t transform;
    int quarter;
} report_pen_t;

static int
text_width(const char* text, int scale) {
    return gfx_font_text_width(gfx_font_ui(), text, -1, scale);
}

/* The string's LOGICAL box is mapped, not its origin, then glyph 0 is placed
 * inside the mapped box - walking glyphs from a mapped point instead drifts
 * the string off at every quarter but the first. */
static void
draw_text(const report_pen_t* pen, int x, int y, const char* text, int scale, gfx_color_t colour) {
    const gfx_font_t* font = gfx_font_ui();
    const mu_Rect box =
        ui_transform_rect(pen->transform, (mu_Rect){x, y, text_width(text, scale), gfx_font_height(font, scale)});

    int gx, gy;
    ui_text_glyph0_origin(font, box, pen->quarter, scale, &gx, &gy);
    gfx_text_font(gx, gy, text, colour, scale, pen->quarter, font);
}

static void
draw_centred(const report_pen_t* pen, mu_Rect where, const char* text, int scale, gfx_color_t colour) {
    draw_text(pen, where.x, where.y, text, scale, colour);
}

static bool
take_line(report_pen_t* pen, mu_Rect* out) {
    const mu_Rect line = post_layout_take_line(&pen->layout, &pen->lines);
    if (line.w <= 0) {
        return false;
    }
    *out = line;
    return true;
}

/* Draws `text` down as many report lines as it needs, indented `indent`
 * characters into each. False once the columns are full. Walks the same
 * wrap the height was measured with, so the two cannot disagree. */
static bool
draw_wrapped(report_pen_t* pen, int indent, const char* text, gfx_color_t colour) {
    char line[POST_WRAP_MAX_CHARS + 1];
    const int columns = post_layout_wrap_columns(&pen->layout, indent);

    int cursor = 0;
    for (;;) {
        const post_wrap_line_t wrapped = post_wrap_next(text, columns, &cursor);
        if (wrapped.len <= 0) {
            return true;
        }

        mu_Rect row;
        if (!take_line(pen, &row)) {
            return false;
        }

        memcpy(line, text + wrapped.start, (size_t)wrapped.len);
        line[wrapped.len] = '\0';
        draw_text(pen, row.x + indent * pen->layout.glyph_w, row.y, line, POST_LAYOUT_REPORT_SCALE, colour);
    }
}

/* What draw_entry() is about to need, so the column can be chosen before any
 * of it is drawn. */
static int
entry_lines(const report_pen_t* pen, const post_result_t* r) {
    if (r->detail[0] == '\0') {
        return 1;
    }
    return 1 + post_wrap_count(r->detail, post_layout_wrap_columns(&pen->layout, MARK_CHARS));
}

static bool
draw_entry(report_pen_t* pen, const post_result_t* r, bool failed) {
    const char* mark;
    gfx_color_t mark_colour;
    if (r->ok) {
        mark = "[ok]";
        mark_colour = gfx_rgb(OK_RGB);
    } else if (r->severity == POST_OPTIONAL) {
        mark = "[--]";
        mark_colour = gfx_rgb(ABSENT_RGB);
    } else {
        mark = "[!!]";
        mark_colour = gfx_rgb(FAIL_RGB);
    }

    mu_Rect row;
    if (!take_line(pen, &row)) {
        return false;
    }

    draw_text(pen, row.x, row.y, mark, POST_LAYOUT_REPORT_SCALE, mark_colour);
    draw_text(pen, row.x + MARK_CHARS * pen->layout.glyph_w, row.y, r->name, POST_LAYOUT_REPORT_SCALE,
              gfx_rgb(HEADING_RGB));

    if (r->detail[0] == '\0') {
        return true;
    }
    return draw_wrapped(pen, MARK_CHARS, r->detail, failed ? gfx_rgb(FAIL_RGB) : gfx_rgb(DETAIL_RGB));
}

static void
draw_results(report_pen_t* pen, bool failures_only) {
    const post_result_t* results = post_results();
    const int count = post_result_count();

    for (int i = 0; i < count; i++) {
        const post_result_t* r = &results[i];

        /* An optional peripheral that is simply absent is not a failure, so
         * it is skipped when only failures were asked for. */
        const bool failed = !r->ok && r->severity == POST_REQUIRED;
        if (failures_only && !failed) {
            continue;
        }

        post_layout_reserve(&pen->layout, &pen->lines, entry_lines(pen, r));
        if (!draw_entry(pen, r, failed)) {
            return;
        }
        post_layout_gap(&pen->layout, &pen->lines);
    }
}

void
post_ui_draw_report(const post_ui_report_t* report) {
    const int quarter = ((report->quarter % 4) + 4) % 4;
    const bool upright = quarter % 2 == 0;
    report_pen_t pen = {
        .layout = post_layout(gfx_font_ui(), upright ? GFX_WIDTH : GFX_HEIGHT, upright ? GFX_HEIGHT : GFX_WIDTH),
        .lines = {0},
        .transform = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT),
        .quarter = quarter,
    };

    gfx_clear(gfx_rgb(BG_RGB));

    const int failures = post_failure_count();
    const gfx_color_t title_colour = failures ? gfx_rgb(FAIL_RGB) : gfx_rgb(HEADING_RGB);
    const int title_w = text_width(report->title, POST_LAYOUT_TITLE_SCALE);
    draw_centred(&pen, post_layout_title_text(&pen.layout, title_w), report->title, POST_LAYOUT_TITLE_SCALE,
                 title_colour);

    char summary[48];
    snprintf(summary, sizeof(summary), "%d checks, %d failed", post_result_count(), failures);
    const int summary_w = text_width(summary, POST_LAYOUT_REPORT_SCALE);
    draw_centred(&pen, post_layout_summary_text(&pen.layout, summary_w), summary, POST_LAYOUT_REPORT_SCALE,
                 failures ? gfx_rgb(FAIL_RGB) : gfx_rgb(OK_RGB));

    draw_results(&pen, report->failures_only);

    if (report->footer != NULL) {
        const int footer_w = text_width(report->footer, POST_LAYOUT_REPORT_SCALE);
        draw_centred(&pen, post_layout_footer_text(&pen.layout, footer_w), report->footer, POST_LAYOUT_REPORT_SCALE,
                     gfx_rgb(DETAIL_RGB));
    }
}

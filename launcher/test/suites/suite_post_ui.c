/*
 * Portable suite: post_layout - the power-on self-test report's geometry.
 *
 * Every assertion is derived from the canvas handed in, so the two panel
 * orientations are the only fixed numbers here and a change to the inset,
 * the gaps or the column count cannot quietly go unasserted.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "boot/post_layout.h"
#include "display/display.h"
#include "gfx/gfx_font_roles.h"

/* The panel, and the panel turned on its side. The logical canvas the report
 * lays out in is one or the other, never anything between. */
#define PANEL_SHORT_SIDE 368
#define PANEL_LONG_SIDE  448

/* The orientation at its column ceiling, with air between checks - what a
 * report that needs every column gets, and the shape the placement rules
 * below are all about. */
static post_layout_t
at_ceiling(int screen_w, int screen_h) {
    return post_layout_with(gfx_font_ui(), screen_w, screen_h, post_layout_max_columns(screen_w, screen_h), 1);
}

static post_layout_t
portrait(void) {
    return at_ceiling(PANEL_SHORT_SIDE, PANEL_LONG_SIDE);
}

static post_layout_t
landscape(void) {
    return at_ceiling(PANEL_LONG_SIDE, PANEL_SHORT_SIDE);
}

static bool
contains(mu_Rect outer, mu_Rect inner) {
    return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w
           && inner.y + inner.h <= outer.y + outer.h;
}

static bool
overlaps(mu_Rect a, mu_Rect b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

static void
assert_safe_area_matches_the_inset(post_layout_t l, int screen_w, int screen_h) {
    const int inset = DISPLAY_PANEL_SAFE_INSET;
    TEST_ASSERT_EQUAL_INT(inset, l.safe.x);
    TEST_ASSERT_EQUAL_INT(inset, l.safe.y);
    TEST_ASSERT_EQUAL_INT(screen_w - 2 * inset, l.safe.w);
    TEST_ASSERT_EQUAL_INT(screen_h - 2 * inset, l.safe.h);
}

static void
test_the_readable_area_insets_every_edge(void) {
    assert_safe_area_matches_the_inset(portrait(), PANEL_SHORT_SIDE, PANEL_LONG_SIDE);
    assert_safe_area_matches_the_inset(landscape(), PANEL_LONG_SIDE, PANEL_SHORT_SIDE);
}

static void
assert_every_line_inside_the_readable_area(post_layout_t l) {
    const int lines = post_layout_capacity(&l);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, lines, "a report with no room for a line is not a layout");

    for (int i = 0; i < lines; i++) {
        const mu_Rect line = post_layout_line(&l, i);
        TEST_ASSERT_GREATER_THAN_INT(0, line.w);
        TEST_ASSERT_TRUE_MESSAGE(contains(l.safe, line), "a report line escaped the panel's readable inset");
        TEST_ASSERT_TRUE_MESSAGE(contains(l.body, line), "a report line escaped the column band");
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, post_layout_line(&l, lines).w, "the line past the last one must read as empty");
}

static void
test_every_line_stays_inside_the_readable_area_in_both_orientations(void) {
    assert_every_line_inside_the_readable_area(portrait());
    assert_every_line_inside_the_readable_area(landscape());
}

static void
assert_no_two_lines_overlap(post_layout_t l) {
    const int lines = post_layout_capacity(&l);
    TEST_ASSERT_GREATER_THAN_INT(0, lines);

    for (int i = 0; i < lines; i++) {
        const mu_Rect a = post_layout_line(&l, i);
        for (int j = i + 1; j < lines; j++) {
            TEST_ASSERT_FALSE_MESSAGE(overlaps(a, post_layout_line(&l, j)), "two report lines share pixels");
        }
    }
}

static void
test_no_two_report_lines_overlap_in_either_orientation(void) {
    assert_no_two_lines_overlap(portrait());
    assert_no_two_lines_overlap(landscape());
}

static void
assert_bands_stack_without_touching(post_layout_t l) {
    TEST_ASSERT_GREATER_THAN_INT(0, l.title.h);
    TEST_ASSERT_GREATER_THAN_INT(0, l.summary.h);
    TEST_ASSERT_GREATER_THAN_INT(0, l.footer.h);
    TEST_ASSERT_GREATER_THAN_INT(0, l.body.h);

    TEST_ASSERT_TRUE(contains(l.safe, l.title));
    TEST_ASSERT_TRUE(contains(l.safe, l.summary));
    TEST_ASSERT_TRUE(contains(l.safe, l.footer));
    TEST_ASSERT_TRUE(contains(l.safe, l.body));

    TEST_ASSERT_FALSE(overlaps(l.title, l.summary));
    TEST_ASSERT_FALSE(overlaps(l.summary, l.body));
    TEST_ASSERT_FALSE(overlaps(l.body, l.footer));
}

static void
test_the_bands_stack_without_touching_in_both_orientations(void) {
    assert_bands_stack_without_touching(portrait());
    assert_bands_stack_without_touching(landscape());
}

static void
assert_centred_horizontally(mu_Rect band, mu_Rect text) {
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, text.w, "centred text was given no width to centre");

    const int left = text.x - band.x;
    const int right = (band.x + band.w) - (text.x + text.w);
    TEST_ASSERT_TRUE_MESSAGE(left >= 0 && right >= 0, "centred text ran outside its band");
    TEST_ASSERT_INT_WITHIN_MESSAGE(1, left, right, "the text is not horizontally centred");
}

static void
assert_title_is_centred(post_layout_t l) {
    const int title_w = gfx_font_text_width(gfx_font_ui(), POST_LAYOUT_TITLE, -1, POST_LAYOUT_TITLE_SCALE);
    assert_centred_horizontally(l.safe, post_layout_title_text(&l, title_w));

    const int fault_w = gfx_font_text_width(gfx_font_ui(), POST_LAYOUT_FAULT_TITLE, -1, POST_LAYOUT_TITLE_SCALE);
    assert_centred_horizontally(l.safe, post_layout_title_text(&l, fault_w));

    const int footer_w = gfx_font_text_width(gfx_font_ui(), POST_LAYOUT_FAULT_FOOTER, -1, POST_LAYOUT_REPORT_SCALE);
    assert_centred_horizontally(l.safe, post_layout_footer_text(&l, footer_w));
}

static void
test_the_title_is_horizontally_centred_in_both_orientations(void) {
    assert_title_is_centred(portrait());
    assert_title_is_centred(landscape());
}

static void
assert_widest_line_fits_a_column(post_layout_t l) {
    const int widest = gfx_font_text_width(gfx_font_ui(), POST_LAYOUT_WIDEST_LINE, -1, POST_LAYOUT_REPORT_SCALE);

    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(widest, l.column_w, "a column is narrower than the widest report line");
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE((int)strlen(POST_LAYOUT_WIDEST_LINE), l.line_chars,
                                             "a line holds fewer characters than the widest report line");

    for (int c = 0; c < l.columns; c++) {
        const mu_Rect column = post_layout_column(&l, c);
        TEST_ASSERT_EQUAL_INT(l.column_w, column.w);
        TEST_ASSERT_TRUE_MESSAGE(contains(l.body, column), "a column escaped the band it divides");
    }
}

static void
test_the_widest_report_line_fits_its_column_in_both_orientations(void) {
    assert_widest_line_fits_a_column(portrait());
    assert_widest_line_fits_a_column(landscape());
}

/* A report laid out the way the drawer lays one out: each check's text lines
 * in a run, a gap asked for after each. Records where the first text line of
 * every column landed, which is what a reader sees as the columns' top edge. */
typedef struct {
    int placed;
    int used_columns;
    int first_y[POST_LAYOUT_LANDSCAPE_COLUMNS];
} placement_t;

static int
column_of(const post_layout_t* l, mu_Rect line) {
    return (line.x - l->body.x) / (l->column_w + l->column_gap);
}

static placement_t
place_report(post_layout_t l, int checks, int lines_each) {
    placement_t p = {0, 0, {0}};
    for (int c = 0; c < POST_LAYOUT_LANDSCAPE_COLUMNS; c++) {
        p.first_y[c] = -1;
    }

    post_lines_t lines = {0};
    for (int check = 0; check < checks; check++) {
        post_layout_reserve(&l, &lines, lines_each);
        for (int i = 0; i < lines_each; i++) {
            const mu_Rect line = post_layout_take_line(&l, &lines);
            if (line.w <= 0) {
                return p;
            }
            TEST_ASSERT_TRUE_MESSAGE(contains(l.body, line), "a placed line escaped the column band");

            const int column = column_of(&l, line);
            if (p.first_y[column] < 0) {
                p.first_y[column] = line.y;
                p.used_columns = column + 1;
            }
            p.placed++;
        }
        post_layout_gap(&l, &lines);
    }
    return p;
}

static void
assert_every_column_starts_on_the_top_row(post_layout_t l, int checks, int lines_each) {
    const placement_t p = place_report(l, checks, lines_each);

    for (int c = 0; c < p.used_columns; c++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(l.body.y, p.first_y[c],
                                      "a column's first text line sits below the body's top row");
    }
}

/* Every run length in turn, so the case the sweep is really after - a check
 * ending exactly on a column's last row, leaving the gap to fall at the top
 * of the next - is covered wherever the divisors put it. */
static void
assert_no_run_length_pushes_a_column_down(post_layout_t l) {
    for (int lines_each = 1; lines_each <= l.rows; lines_each++) {
        assert_every_column_starts_on_the_top_row(l, post_layout_capacity(&l), lines_each);
    }
}

static void
test_no_column_starts_on_a_gap_in_either_orientation(void) {
    assert_no_run_length_pushes_a_column_down(portrait());
    assert_no_run_length_pushes_a_column_down(landscape());
}

/* The reported symptom, built on purpose rather than waited for: the pass
 * filled to a column's last row, so the gap asked for next has nowhere to go
 * but the following column's first row. Needs a second column to land in, so
 * portrait reaches it through the sweep above instead. */
static void
assert_a_check_ending_on_the_last_row_costs_nothing(post_layout_t l) {
    if (l.columns < 2) {
        return;
    }

    post_lines_t lines = {0};
    for (int i = 0; i < l.rows; i++) {
        TEST_ASSERT_GREATER_THAN_INT(0, post_layout_take_line(&l, &lines).w);
    }
    post_layout_gap(&l, &lines);

    const mu_Rect first = post_layout_take_line(&l, &lines);
    TEST_ASSERT_GREATER_THAN_INT(0, first.w);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, column_of(&l, first), "the pass should have moved on to the second column");
    TEST_ASSERT_EQUAL_INT_MESSAGE(l.body.y, first.y,
                                  "the gap after a check ending on a column's last row pushed the next column down");
}

static void
test_a_check_ending_on_a_column_boundary_does_not_shift_the_next_column(void) {
    assert_a_check_ending_on_the_last_row_costs_nothing(portrait());
    assert_a_check_ending_on_the_last_row_costs_nothing(landscape());
}

/* Swallowing a gap at a column boundary must not cost a line anywhere else:
 * a report with room to spare still gets every line it asked for. */
static void
assert_a_report_that_fits_loses_no_line(post_layout_t l) {
    const int lines_each = 3;
    const int checks = post_layout_capacity(&l) / (lines_each + 1);
    TEST_ASSERT_GREATER_THAN_INT(0, checks);

    const placement_t p = place_report(l, checks, lines_each);
    TEST_ASSERT_EQUAL_INT_MESSAGE(checks * lines_each, p.placed, "a report that fits lost text lines");
}

static void
test_a_report_that_fits_loses_no_line_in_either_orientation(void) {
    assert_a_report_that_fits_loses_no_line(portrait());
    assert_a_report_that_fits_loses_no_line(landscape());
}

/* The pass stops where post_layout_line() does, so the two cannot disagree
 * about how much the columns hold. */
static void
assert_the_pass_stops_at_capacity(post_layout_t l) {
    post_lines_t lines = {0};
    int placed = 0;
    while (post_layout_take_line(&l, &lines).w > 0) {
        placed++;
        TEST_ASSERT_LESS_OR_EQUAL_INT(post_layout_capacity(&l), placed);
    }
    TEST_ASSERT_EQUAL_INT(post_layout_capacity(&l), placed);
}

static void
test_the_pass_places_exactly_the_capacity_in_either_orientation(void) {
    assert_the_pass_stops_at_capacity(portrait());
    assert_the_pass_stops_at_capacity(landscape());
}

/* One check placed the way the drawer places one: its height reserved, then
 * its lines taken. */
typedef struct {
    mu_Rect first;
    mu_Rect last;
    int placed;
} entry_placement_t;

static entry_placement_t
place_entry(const post_layout_t* l, post_lines_t* lines, int height) {
    entry_placement_t e = {{0, 0, 0, 0}, {0, 0, 0, 0}, 0};

    post_layout_reserve(l, lines, height);
    for (int i = 0; i < height; i++) {
        const mu_Rect line = post_layout_take_line(l, lines);
        if (line.w <= 0) {
            break;
        }
        if (e.placed == 0) {
            e.first = line;
        }
        e.last = line;
        e.placed++;
    }
    return e;
}

/* Fills the pass to exactly `remaining` rows short of the column's end. */
static post_lines_t
filled_to_remainder(const post_layout_t* l, int remaining) {
    post_lines_t lines = {0};
    for (int i = 0; i < l->rows - remaining; i++) {
        TEST_ASSERT_GREATER_THAN_INT(0, post_layout_take_line(l, &lines).w);
    }
    return lines;
}

/* A check one line taller than the column has left for it: it belongs at the
 * top of the next column, and the rows it skipped stay empty. */
static void
assert_a_check_that_does_not_fit_moves_whole(post_layout_t l) {
    if (l.columns < 2) {
        return; /* nowhere to move to - see the single-column test */
    }

    for (int height = 2; height <= l.rows; height++) {
        post_lines_t lines = filled_to_remainder(&l, height - 1);
        const entry_placement_t e = place_entry(&l, &lines, height);

        TEST_ASSERT_EQUAL_INT_MESSAGE(height, e.placed, "the moved check lost lines");
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, column_of(&l, e.first), "a check that did not fit stayed in the full column");
        TEST_ASSERT_EQUAL_INT_MESSAGE(l.body.y, e.first.y, "the moved check did not start at the column top");
        TEST_ASSERT_EQUAL_INT_MESSAGE(column_of(&l, e.first), column_of(&l, e.last), "the moved check still split");
    }
}

static void
test_a_check_that_does_not_fit_the_remainder_moves_whole_to_the_next_column(void) {
    assert_a_check_that_does_not_fit_moves_whole(portrait());
    assert_a_check_that_does_not_fit_moves_whole(landscape());
}

/* A check with exactly the room it needs stays where it is - the rule must
 * not spend a column break it does not owe. */
static void
assert_a_check_that_exactly_fits_stays(post_layout_t l) {
    for (int height = 1; height <= l.rows; height++) {
        post_lines_t lines = filled_to_remainder(&l, height);
        const entry_placement_t e = place_entry(&l, &lines, height);

        TEST_ASSERT_EQUAL_INT(height, e.placed);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, column_of(&l, e.first), "a check that fitted was moved anyway");
        TEST_ASSERT_EQUAL_INT_MESSAGE(l.body.y + (l.rows - height) * l.line_h, e.first.y,
                                      "a check that fitted did not start where the column had left off");
    }
}

static void
test_a_check_that_exactly_fits_stays_in_its_column(void) {
    assert_a_check_that_exactly_fits_stays(portrait());
    assert_a_check_that_exactly_fits_stays(landscape());
}

/* Taller than any column, so it cannot be kept whole: it starts at a column
 * top and runs on from there. */
static void
assert_a_check_taller_than_a_column_starts_at_a_top(post_layout_t l) {
    if (l.columns < 2) {
        return;
    }

    post_lines_t lines = {0};
    TEST_ASSERT_GREATER_THAN_INT(0, post_layout_take_line(&l, &lines).w); /* off a column top */

    const entry_placement_t e = place_entry(&l, &lines, l.rows + 1);

    TEST_ASSERT_EQUAL_INT_MESSAGE(l.rows + 1, e.placed, "the over-tall check lost lines");
    TEST_ASSERT_EQUAL_INT_MESSAGE(l.body.y, e.first.y, "an over-tall check did not start at a column top");
    TEST_ASSERT_EQUAL_INT_MESSAGE(column_of(&l, e.first) + 1, column_of(&l, e.last),
                                  "an over-tall check should run into the next column");
}

static void
test_a_check_taller_than_a_column_starts_at_a_column_top_and_splits(void) {
    assert_a_check_taller_than_a_column_starts_at_a_top(landscape());
}

/* Portrait has one column, so there is never a next one to move into: the
 * rule must leave the pass exactly where it found it rather than skip to a
 * top that does not exist and drop the rest of the report. */
static void
test_a_single_column_report_never_skips_rows(void) {
    post_layout_t l = portrait();
    TEST_ASSERT_EQUAL_INT(1, l.columns);

    for (int height = 1; height <= l.rows; height++) {
        post_lines_t lines = filled_to_remainder(&l, height - 1);
        const int before = lines.next;
        post_layout_reserve(&l, &lines, height);
        TEST_ASSERT_EQUAL_INT_MESSAGE(before, lines.next, "a single-column report skipped rows it could have used");
    }
}

static const char* const wrap_samples[] = {
    "",
    "a",
    "short",
    "aa:bb:cc:dd:ee:ff",
    "rev 0, 2 core, wifi ble ",
    "183 KiB free, DMA block 180 KiB",
    "0x20  TCA9554 reset lines",
    "port 0, SDA 15, SCL 14",
    "SD64G, 59640 MB, mounted and released",
    "supercalifragilisticexpialidocious",
    "  leading and  doubled   spaces  ",
    POST_LAYOUT_WIDEST_LINE,
};

/* The count and the walk are the same routine, so a drawer that walks it
 * consumes exactly the lines the measure promised - and the walk covers the
 * whole string, dropping nothing but the spaces it breaks on. */
static void
assert_the_wrap_walk_matches_its_count(const char* text, int columns) {
    int cursor = 0;
    int emitted = 0;

    for (;;) {
        const post_wrap_line_t line = post_wrap_next(text, columns, &cursor);
        if (line.len <= 0) {
            break;
        }
        TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, line.len, "a wrapped line made no progress");
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE(columns, line.len, "a wrapped line ran past the column");
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE((int)strlen(text), line.start + line.len, "a wrapped line ran past it");
        emitted++;
        TEST_ASSERT_LESS_OR_EQUAL_INT_MESSAGE((int)strlen(text), emitted, "the wrap walk did not terminate");
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(emitted, post_wrap_count(text, columns), "the measure and the walk disagree");

    while (text[cursor] == ' ') {
        cursor++;
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)strlen(text), cursor, "the wrap walk left part of the string unplaced");
}

static void
test_the_wrap_measure_and_walk_agree_at_every_width(void) {
    const int widest = landscape().line_chars > portrait().line_chars ? landscape().line_chars : portrait().line_chars;

    for (size_t s = 0; s < sizeof(wrap_samples) / sizeof(wrap_samples[0]); s++) {
        for (int columns = 1; columns <= widest; columns++) {
            assert_the_wrap_walk_matches_its_count(wrap_samples[s], columns);
        }
    }
}

/* The width a detail is measured at is the width it is drawn at, and never
 * wider than the drawer's own line buffer. */
static void
assert_the_wrap_width_is_drawable(post_layout_t l) {
    for (int indent = 0; indent <= l.line_chars + 1; indent++) {
        const int columns = post_layout_wrap_columns(&l, indent);
        TEST_ASSERT_GREATER_THAN_INT(0, columns);
        TEST_ASSERT_LESS_OR_EQUAL_INT(POST_WRAP_MAX_CHARS, columns);
        TEST_ASSERT_LESS_OR_EQUAL_INT(l.line_chars, columns);
    }
}

static void
test_the_wrap_width_is_always_drawable_in_both_orientations(void) {
    assert_the_wrap_width_is_drawable(portrait());
    assert_the_wrap_width_is_drawable(landscape());
}

/* Synthetic checks: a list of detail strings, which is all the chooser needs
 * to measure a report. Nothing here stands in for a board's own answers. */
typedef struct {
    int count;
    const char* const* details;
} listed_t;

static const char*
listed_detail(void* ctx, int index) {
    const listed_t* l = ctx;
    return l->details[index % l->count];
}

static post_entries_t
listed_entries(listed_t* list, int count) {
    const post_entries_t entries = {.count = count, .detail = listed_detail, .ctx = list};
    return entries;
}

/* Detail strings around the length a check's format string produces - short
 * enough to sit on one line in a full-width column, long enough to wrap in a
 * narrow one. */
static const char* const report_details[] = {
    "SDCARD, 29820 MB (live, 51 ms round trip)",
    "rev 2, 2 core, wifi ble",
    "16 MB",
    "140 KiB free, DMA block 76 KiB",
    "8 MiB present",
    "90:70:69:fe:a3:08",
    "38.5 C",
    "port 0, SDA 15, SCL 14",
    "0x20  TCA9554 reset lines",
    "0x34  AXP2101 power",
    "0x6b  QMI8658 accel+gyro",
    "0x51  PCF85063 clock",
    "0x15  CST820 (V2)",
    "0x18  ES8311",
    "368x448 CO5300 + CST820 (V2)",
};

#define REPORT_CHECKS ((int)(sizeof(report_details) / sizeof(report_details[0])))

/* Long enough that only the narrowest columns hold it in few lines - what a
 * report needs before it wants every column the orientation allows. */
static const char* const tall_details[] = {
    "a detail long enough that it wraps to several lines even across a very wide column indeed",
};

static const char* const short_details[] = {
    "absent - unexpected",
    "no controller answered",
};

/* Walks the chosen layout the way the drawer walks it, reporting how many
 * checks got all their lines and how the placed ones were spaced. */
typedef struct {
    int placed_checks;
    int columns_used;
    int max_same_column_spacing;
    int min_same_column_spacing;
} report_walk_t;

static report_walk_t
walk_report(const post_layout_t* l, const post_entries_t* entries) {
    report_walk_t w = {0, 0, -1, -1};

    post_lines_t lines = {0};
    int previous_end = -1;
    int previous_column = -1;

    for (int i = 0; i < entries->count; i++) {
        const int height = post_layout_entry_height(l, entries->detail(entries->ctx, i));
        post_layout_reserve(l, &lines, height);

        mu_Rect first = {0, 0, 0, 0};
        int placed = 0;
        for (int line = 0; line < height; line++) {
            const mu_Rect row = post_layout_take_line(l, &lines);
            if (row.w <= 0) {
                break;
            }
            if (placed == 0) {
                first = row;
            }
            placed++;
        }
        if (placed < height) {
            return w;
        }

        const int column = column_of(l, first);
        const int start = lines.next - height;
        if (previous_end >= 0 && column == previous_column) {
            const int spacing = start - previous_end;
            if (w.min_same_column_spacing < 0 || spacing < w.min_same_column_spacing) {
                w.min_same_column_spacing = spacing;
            }
            if (spacing > w.max_same_column_spacing) {
                w.max_same_column_spacing = spacing;
            }
        }
        previous_end = lines.next;
        previous_column = column;

        w.placed_checks++;
        w.columns_used = column + 1;
        post_layout_gap(l, &lines);
    }
    return w;
}

/* One spacing for the whole report, not a decision taken per check. */
static void
test_the_spacing_is_uniform_across_a_report(void) {
    listed_t list = {REPORT_CHECKS, report_details};

    const int canvases[][2] = {{PANEL_SHORT_SIDE, PANEL_LONG_SIDE}, {PANEL_LONG_SIDE, PANEL_SHORT_SIDE}};
    for (int c = 0; c < 2; c++) {
        const post_entries_t entries = listed_entries(&list, REPORT_CHECKS);
        const post_layout_t l = post_layout_for_report(gfx_font_ui(), canvases[c][0], canvases[c][1], &entries);
        const report_walk_t w = walk_report(&l, &entries);

        if (w.min_same_column_spacing < 0) {
            continue; /* every check started a column of its own */
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(w.min_same_column_spacing, w.max_same_column_spacing,
                                      "checks in one report were spaced differently");
        TEST_ASSERT_EQUAL_INT_MESSAGE(l.gap, w.min_same_column_spacing - 0, "the spacing drawn is not the one chosen");
    }
}

/* Two failures do not need three narrow columns, and a wide one wraps their
 * details far less - which is the whole reason to prefer fewer. */
static void
test_a_short_report_in_landscape_takes_one_wide_column(void) {
    listed_t list = {2, short_details};
    const post_entries_t entries = listed_entries(&list, 2);
    const post_layout_t chosen = post_layout_for_report(gfx_font_ui(), PANEL_LONG_SIDE, PANEL_SHORT_SIDE, &entries);
    const post_layout_t ceiling = landscape();

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, chosen.columns, "a two-check report was split into columns it did not need");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(ceiling.line_chars, chosen.line_chars, "one column should be the wider one");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(post_layout_entry_height(&ceiling, short_details[1]),
                                      post_layout_entry_height(&chosen, short_details[1]),
                                      "the wider column should wrap the detail into fewer lines");
}

/* A report tall enough to need them still gets every column. */
static void
test_a_tall_report_in_landscape_takes_the_orientation_ceiling(void) {
    listed_t list = {1, tall_details};
    const post_entries_t entries = listed_entries(&list, REPORT_CHECKS);
    const post_layout_t l = post_layout_for_report(gfx_font_ui(), PANEL_LONG_SIDE, PANEL_SHORT_SIDE, &entries);

    TEST_ASSERT_EQUAL_INT_MESSAGE(post_layout_max_columns(PANEL_LONG_SIDE, PANEL_SHORT_SIDE), l.columns,
                                  "a report that needs every column did not get them");
}

static void
test_the_column_count_follows_the_orientation(void) {
    TEST_ASSERT_EQUAL_INT(POST_LAYOUT_PORTRAIT_COLUMNS, portrait().columns);
    TEST_ASSERT_EQUAL_INT(POST_LAYOUT_LANDSCAPE_COLUMNS, landscape().columns);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(portrait().columns, landscape().columns,
                                         "the short side is what forces columns");
}

/* Landscape loses height for the same report, so the columns have to buy
 * those lines back - otherwise the split has not paid for itself. */
static void
test_landscape_holds_at_least_as_many_lines_as_portrait(void) {
    const post_layout_t tall = portrait();
    const post_layout_t wide = landscape();

    TEST_ASSERT_GREATER_THAN_INT(0, post_layout_capacity(&tall));
    TEST_ASSERT_EQUAL_INT(tall.rows * tall.columns, post_layout_capacity(&tall));
    TEST_ASSERT_GREATER_OR_EQUAL_INT_MESSAGE(post_layout_capacity(&tall), post_layout_capacity(&wide),
                                             "the landscape columns hold fewer lines than one portrait column");
}

void
suite_post_ui(void) {
    RUN_TEST(test_the_readable_area_insets_every_edge);
    RUN_TEST(test_every_line_stays_inside_the_readable_area_in_both_orientations);
    RUN_TEST(test_no_two_report_lines_overlap_in_either_orientation);
    RUN_TEST(test_the_bands_stack_without_touching_in_both_orientations);
    RUN_TEST(test_the_title_is_horizontally_centred_in_both_orientations);
    RUN_TEST(test_the_widest_report_line_fits_its_column_in_both_orientations);
    RUN_TEST(test_no_column_starts_on_a_gap_in_either_orientation);
    RUN_TEST(test_a_check_ending_on_a_column_boundary_does_not_shift_the_next_column);
    RUN_TEST(test_a_report_that_fits_loses_no_line_in_either_orientation);
    RUN_TEST(test_the_pass_places_exactly_the_capacity_in_either_orientation);
    RUN_TEST(test_a_check_that_does_not_fit_the_remainder_moves_whole_to_the_next_column);
    RUN_TEST(test_a_check_that_exactly_fits_stays_in_its_column);
    RUN_TEST(test_a_check_taller_than_a_column_starts_at_a_column_top_and_splits);
    RUN_TEST(test_a_single_column_report_never_skips_rows);
    RUN_TEST(test_the_wrap_measure_and_walk_agree_at_every_width);
    RUN_TEST(test_the_wrap_width_is_always_drawable_in_both_orientations);
    RUN_TEST(test_the_spacing_is_uniform_across_a_report);
    RUN_TEST(test_a_short_report_in_landscape_takes_one_wide_column);
    RUN_TEST(test_a_tall_report_in_landscape_takes_the_orientation_ceiling);
    RUN_TEST(test_the_column_count_follows_the_orientation);
    RUN_TEST(test_landscape_holds_at_least_as_many_lines_as_portrait);
}

SUITE_REGISTER(suite_post_ui);

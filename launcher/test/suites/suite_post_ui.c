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

static post_layout_t
portrait(void) {
    return post_layout(gfx_font_ui(), PANEL_SHORT_SIDE, PANEL_LONG_SIDE);
}

static post_layout_t
landscape(void) {
    return post_layout(gfx_font_ui(), PANEL_LONG_SIDE, PANEL_SHORT_SIDE);
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
    RUN_TEST(test_the_column_count_follows_the_orientation);
    RUN_TEST(test_landscape_holds_at_least_as_many_lines_as_portrait);
}

SUITE_REGISTER(suite_post_ui);

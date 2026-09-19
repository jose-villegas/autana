/*
 * post_layout - where the power-on self-test report's title, summary and
 * report lines sit on the panel.
 *
 * Pure geometry with the canvas passed in, so the same rects resolve on a
 * host as on the board; post_ui.c owns the drawing. Arithmetic rather than a
 * UI layout because this report is drawn before the frame loop - and the UI
 * layer's screens with it - exists.
 *
 * Rects resolve in the UPRIGHT LOGICAL canvas, width and height already
 * swapped for whichever quarter turn the panel is read at, and the drawer
 * maps them onto the panel.
 */
#pragma once

#include "ui/ui_anchor.h"

/* The screen's fixed strings, beside the rects that have to hold them. */
#define POST_LAYOUT_TITLE             "POWER-ON SELF TEST"
#define POST_LAYOUT_FAULT_TITLE       "HARDWARE FAULT"
#define POST_LAYOUT_FAULT_FOOTER      "touch to continue"

/* The widest line a column holds unwrapped: a status mark and the longest
 * check name. A canvas too narrow for it at the orientation's column count
 * gives up a column rather than truncate; a longer detail string wraps. */
#define POST_LAYOUT_WIDEST_LINE       "[ok] audio codec"

/* The MOST columns an orientation will split into - landscape's short side
 * is what forces any at all, portrait has the height for a single list. A
 * report uses the fewest it fits in, which is rarely the ceiling. */
#define POST_LAYOUT_LANDSCAPE_COLUMNS 3
#define POST_LAYOUT_PORTRAIT_COLUMNS  1

/* The status mark and the space after it: where a check's name begins, and
 * how far its detail lines are indented. */
#define POST_LAYOUT_MARK_CHARS        5

/* Report text at 8x8 glyphs: at the UI's own scale the panel holds 23
 * characters, narrower than most of the detail strings. The title takes the
 * UI scale so it reads as a heading. */
#define POST_LAYOUT_REPORT_SCALE      1
#define POST_LAYOUT_TITLE_SCALE       2

typedef struct {
    mu_Rect safe;    /* what the panel's glass leaves readable */
    mu_Rect title;   /* full-width band; the text centres within it */
    mu_Rect summary; /* the same, one report line tall */
    mu_Rect footer;  /* the same, along the bottom */
    mu_Rect body;    /* the band the columns divide between them */
    int columns;
    int column_w;
    int column_gap;
    int glyph_w;
    int line_h;
    int rows;       /* text lines a column holds with nothing between them */
    int line_chars; /* characters a line holds before it has to wrap */
    int gap;        /* PIXELS of air between checks, 0 to one line_h */
} post_layout_t;

/* The layout at an explicit column count and spacing. `screen_w`/`screen_h`
 * are the upright logical canvas; `font` supplies the metrics both scales
 * above are measured in. A column count too wide for the canvas is reduced
 * until POST_LAYOUT_WIDEST_LINE fits. Without a footer, that band takes no
 * height at all and the columns get it. */
post_layout_t post_layout_with(const gfx_font_t* font, int screen_w, int screen_h, int columns, int gap,
                               bool has_footer);

/* The orientation's column ceiling. */
int post_layout_max_columns(int screen_w, int screen_h);

/* A `text_w` wide run of text, centred in the band each one names. */
mu_Rect post_layout_title_text(const post_layout_t* l, int text_w);
mu_Rect post_layout_summary_text(const post_layout_t* l, int text_w);
mu_Rect post_layout_footer_text(const post_layout_t* l, int text_w);

mu_Rect post_layout_column(const post_layout_t* l, int column);

/* Report line `index`, filling one column top to bottom before starting the
 * next. Empty once the columns are full, which is the drawer's cue to stop. */
mu_Rect post_layout_line(const post_layout_t* l, int index);

/* Text lines the columns hold with nothing between them - what
 * post_layout_line() addresses, and the most a report can ever show. */
int post_layout_capacity(const post_layout_t* l);

/* The same room measured the way the pass actually spends it: pixels of
 * column, across every column. Air between checks comes out of this, and a
 * row number cannot express it. */
int post_layout_capacity_px(const post_layout_t* l);

/* One pass down the columns, keeping its place as a PIXEL offset into the
 * column it is in - the air between checks is measured in pixels, so a row
 * index no longer addresses a line. The layout stays immutable; this is the
 * only thing that moves. */
typedef struct {
    int column;
    int y;
} post_lines_t;

/* The line to draw the next run of text on, advancing the pass. Empty once
 * the columns are full. */
mu_Rect post_layout_take_line(const post_layout_t* l, post_lines_t* lines);

/* Asks for the air between one check and the next, which is nothing at all
 * when the report only fits without it. A gap that would fall at the top of
 * a column is dropped, so every column's first line carries text. */
void post_layout_gap(const post_layout_t* l, post_lines_t* lines);

/* Asks for `n` lines to be placed together, before any is taken. A check that
 * does not fit what is left of the column starts the next one instead, so its
 * detail is never orphaned at a column's head under no name. One taller than
 * a whole column starts at a column top and splits. Nothing moves with no
 * column left to move into. */
void post_layout_reserve(const post_layout_t* l, post_lines_t* lines, int n);

/* The longest wrapped line the drawer copies out in one piece. Both the
 * measure and the draw take their width from post_layout_wrap_columns(), so
 * neither can ask for more than the other can render. */
#define POST_WRAP_MAX_CHARS 63

/* Characters a line holds `indent` characters into a column. */
int post_layout_wrap_columns(const post_layout_t* l, int indent);

/* One wrapped line: `len` characters from `start` in the text. */
typedef struct {
    int start;
    int len;
} post_wrap_line_t;

/* Walks `text` one wrapped line at a time at `columns` wide, breaking at the
 * last space that still fits and hard-breaking a token longer than the line -
 * so a pathological string still renders rather than looping forever. Start
 * `cursor` at 0; a returned `len` of 0 ends the walk. */
post_wrap_line_t post_wrap_next(const char* text, int columns, int* cursor);

/* The same walk, counted rather than drawn - which is how a check's height
 * is known before a line of it is placed. */
int post_wrap_count(const char* text, int columns);

/* Report lines a check needs here: its mark-and-name line, plus however many
 * its detail wraps to at THIS layout's width. Narrower columns wrap more, so
 * a check's height is a property of the layout, not of the check. */
int post_layout_entry_height(const post_layout_t* l, const char* detail);

/* The checks a report is about to draw, without committing to how they are
 * stored - all the layout needs is each one's detail string, to measure. */
typedef struct {
    int count;
    const char* (*detail)(void* ctx, int index);
    void* ctx;
    bool has_footer;
} post_entries_t;

/* The layout to draw this report with: the fewest columns it fits in with a
 * full line of air between checks, since fewer columns are wider ones and
 * wrap the details less. Where no column count affords that, every column is
 * used and the air shrinks to the widest uniform pixel gap that still fits -
 * air thinner than a line beats dropping checks off the end. */
post_layout_t post_layout_for_report(const gfx_font_t* font, int screen_w, int screen_h, const post_entries_t* entries);

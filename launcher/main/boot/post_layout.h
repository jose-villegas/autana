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

/* Landscape's short side is what forces columns at all; portrait has the
 * height for a single list. */
#define POST_LAYOUT_LANDSCAPE_COLUMNS 3
#define POST_LAYOUT_PORTRAIT_COLUMNS  1

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
    int rows;       /* report lines one column holds */
    int line_chars; /* characters a line holds before it has to wrap */
} post_layout_t;

/* `screen_w` and `screen_h` are the upright logical canvas; `font` supplies
 * the text metrics both scales above are measured in. */
post_layout_t post_layout(const gfx_font_t* font, int screen_w, int screen_h);

/* A `text_w` wide run of text, centred in the band each one names. */
mu_Rect post_layout_title_text(const post_layout_t* l, int text_w);
mu_Rect post_layout_summary_text(const post_layout_t* l, int text_w);
mu_Rect post_layout_footer_text(const post_layout_t* l, int text_w);

mu_Rect post_layout_column(const post_layout_t* l, int column);

/* Report line `index`, filling one column top to bottom before starting the
 * next. Empty once the columns are full, which is the drawer's cue to stop. */
mu_Rect post_layout_line(const post_layout_t* l, int index);

int post_layout_capacity(const post_layout_t* l);

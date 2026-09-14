/*
 * gfx_font - a font DESCRIPTOR, so gfx can carry more than one font.
 *
 * Pure, like gfx_color.h: no gfx.h, no BSP, no drivers. That is what lets a
 * font's metrics be computed and tested on a host, the same way a colour
 * table can be built and tested without the panel - see gfx_color.h's own
 * top comment for why that split matters on this project.
 *
 * Two fonts exist: the 8x8 bitmap in font8x8_basic.h, wrapped below as
 * gfx_font_8x8 so it is an ordinary entry in this scheme rather than a
 * special case something else routes around, and an 8bpp coverage atlas
 * with anti-aliasing and real proportional advances - generated from a TTF
 * by tools/gen_font.py, e.g. main/gfx/fonts/font_lmroman_40.h. `bpp` is a
 * field and advances are per-glyph-capable so an atlas font like that slots
 * in as an ordinary gfx_font_t with no change here. See gfx.c's
 * draw_glyph_font() for how a bpp==8 atlas is actually drawn (blended, not
 * masked); this file stays pure metrics, no drawing, so both fonts'
 * widths/advances/heights are computable and testable on a host with
 * neither gfx.h nor a framebuffer.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h> /* NULL, used by the advance field of a monospace font */
#include <stdint.h>

#include "gfx/font8x8_basic.h"

/* Describes one font: where its glyph bitmaps live, how big a cell is,
 * which codepoints it covers, and cursor advance per glyph. `atlas` is
 * const so it lands in flash at zero RAM. Glyphs are packed cell by
 * cell from `first`; a 1bpp glyph is `cell_h` bytes, one per row, bit 0
 * (LSB) the LEFTMOST pixel - see gfx_font_8x8 below. An 8bpp coverage
 * atlas uses `cell_w * cell_h` bytes per glyph, one byte per pixel,
 * row-major, 0..255 background to ink - see tools/gen_font.py and
 * gfx.c's draw_glyph_font(). */
typedef struct {
    const uint8_t* atlas;   /* glyph bitmaps, cell by cell */
    uint8_t bpp;            /* 1 = bitmask (gfx_font_8x8); 8 = coverage */
    uint8_t cell_w, cell_h; /* one glyph's cell, in atlas pixels */
    uint8_t first;          /* first codepoint the atlas covers */
    uint16_t count;         /* how many glyphs follow it, from `first` */
    const uint8_t* advance; /* per-glyph advance in pixels, indexed from
                                 * `first`; NULL means monospace at cell_w */
} gfx_font_t;

/* The existing 8x8 font, as an ordinary gfx_font_t. font8x8_basic
 * covers U+0000-U+007F, one row of bits per byte, bit 0 (LSB) the
 * leftmost pixel - exactly the layout `atlas` above promises for a
 * 1bpp font, so the table is pointed at directly with no repacking.
 * Monospace: `advance` is NULL, so every glyph advances by cell_w - see
 * gfx_font_advance() below. `static const`, not `extern`: this header
 * is pure and included from more than one translation unit, and
 * internal linkage keeps that safe. */
static const gfx_font_t gfx_font_8x8 = {
    .atlas = (const uint8_t*)font8x8_basic,
    .bpp = 1,
    .cell_w = 8,
    .cell_h = 8,
    .first = 0,
    .count = 128,
    .advance = NULL,
};

/* How far the cursor moves for one glyph of `ch`, at `scale`. A
 * codepoint outside [f->first, f->first + f->count) - or any codepoint
 * at all, for a monospace font - falls back to cell_w * scale. That
 * matches the font this module ships today exactly, where
 * gfx_text_turned() advanced by a fixed cell every character regardless
 * of range; see gfx.c. A font WITH an advance table looks up the
 * per-glyph value only when `ch` is in range, and falls back the same
 * way otherwise. */
static inline int
gfx_font_advance(const gfx_font_t* f, unsigned char ch, int scale) {
    if (scale < 1) {
        scale = 1;
    }
    if (f->advance != NULL && ch >= f->first && (unsigned)(ch - f->first) < f->count) {
        return f->advance[ch - f->first] * scale;
    }
    return f->cell_w * scale;
}

/* Width in pixels of `len` characters of `s`, at `scale`. `len < 0`
 * means NUL-terminated, same contract gfx_text_width() has. For a
 * monospace font (advance == NULL) this is len * cell_w * scale and
 * never looks at `s`'s content, matching gfx_text_width() exactly - a
 * caller passing a `len` longer than what `s` holds is not reading past
 * it today, and must not start to just because this got more general. A
 * font with a real advance table has no such shortcut and reads each
 * character. */
static inline int
gfx_font_text_width(const gfx_font_t* f, const char* s, int len, int scale) {
    if (scale < 1) {
        scale = 1;
    }
    if (len < 0) {
        len = 0;
        while (s[len] != '\0') {
            len++;
        }
    }
    if (f->advance == NULL) {
        return len * f->cell_w * scale;
    }
    int width = 0;
    for (int i = 0; i < len; i++) {
        width += gfx_font_advance(f, (unsigned char)s[i], scale);
    }
    return width;
}

/* Height in pixels of one line of `f`, at `scale`. */
static inline int
gfx_font_height(const gfx_font_t* f, int scale) {
    if (scale < 1) {
        scale = 1;
    }
    return f->cell_h * scale;
}

/* Screen-space rect for glyph columns [col0, col1] of one row, at `turn`
 * (numbered as display.h) and `scale`. A quarter turn maps one glyph
 * axis onto one screen axis, so a run of set bits within a row is always
 * a straight span in screen space too - one rect instead of one per bit.
 * col0 <= col1 required. */
static inline void
gfx_font_row_run_rect(const gfx_font_t* f, int x, int y, int row, int col0, int col1, int scale, int turn, int* out_x,
                      int* out_y, int* out_w, int* out_h) {
    const int run = col1 - col0 + 1;
    switch (turn) {
        case 1: /* top-to-bottom: glyph row fixes screen x, columns run down y */
            *out_x = x + (f->cell_h - 1 - row) * scale;
            *out_y = y + col0 * scale;
            *out_w = scale;
            *out_h = run * scale;
            break;
        case 2: /* upside down: glyph row fixes screen y, columns run backward along x */
            *out_x = x + (f->cell_w - 1 - col1) * scale;
            *out_y = y + (f->cell_h - 1 - row) * scale;
            *out_w = run * scale;
            *out_h = scale;
            break;
        case 3: /* bottom-to-top: glyph row fixes screen x, columns run backward up y */
            *out_x = x + row * scale;
            *out_y = y + (f->cell_w - 1 - col1) * scale;
            *out_w = scale;
            *out_h = run * scale;
            break;
        default: /* upright: glyph row fixes screen y, columns run along x */
            *out_x = x + col0 * scale;
            *out_y = y + row * scale;
            *out_w = run * scale;
            *out_h = scale;
            break;
    }
}

/* gfx_font_row_run_rect(), grown by one pixel on every side - unioning a
 * rect's 8 unit-offset copies (ui_style.h's UI_TEXT_OUTLINED) covers the
 * same area as this, since those 8 offsets are a full 3x3 neighbourhood
 * minus its own centre, which the caller redraws in ink afterwards
 * anyway. */
static inline void
gfx_font_row_run_rect_dilated(const gfx_font_t* f, int x, int y, int row, int col0, int col1, int scale, int turn,
                              int* out_x, int* out_y, int* out_w, int* out_h) {
    gfx_font_row_run_rect(f, x, y, row, col0, col1, scale, turn, out_x, out_y, out_w, out_h);
    *out_x -= 1;
    *out_y -= 1;
    *out_w += 2;
    *out_h += 2;
}

/* gfx_font_row_run_rect(), generalised from one glyph row to a row RANGE
 * [row0, row1) sharing the same [col0, col1] run - a vertical stroke
 * spans several rows with an identical run, and mapping the whole box at
 * once, rather than row by row, is what turns landscape's "one narrow
 * rect per row" into one rect regardless of turn. Reduces to
 * gfx_font_row_run_rect() when row1 == row0 + 1. */
static inline void
gfx_font_run_box_rect(const gfx_font_t* f, int x, int y, int row0, int row1, int col0, int col1, int scale, int turn,
                      int* out_x, int* out_y, int* out_w, int* out_h) {
    const int rows = row1 - row0;
    const int cols = col1 - col0 + 1;
    switch (turn) {
        case 1: /* glyph rows run backward along screen x, columns run down y */
            *out_x = x + (f->cell_h - row1) * scale;
            *out_y = y + col0 * scale;
            *out_w = rows * scale;
            *out_h = cols * scale;
            break;
        case 2: /* columns run backward along x, rows run backward along y */
            *out_x = x + (f->cell_w - 1 - col1) * scale;
            *out_y = y + (f->cell_h - row1) * scale;
            *out_w = cols * scale;
            *out_h = rows * scale;
            break;
        case 3: /* rows run along screen x, columns run backward along y */
            *out_x = x + row0 * scale;
            *out_y = y + (f->cell_w - 1 - col1) * scale;
            *out_w = rows * scale;
            *out_h = cols * scale;
            break;
        default: /* upright: columns run along x, rows run along y */
            *out_x = x + col0 * scale;
            *out_y = y + row0 * scale;
            *out_w = cols * scale;
            *out_h = rows * scale;
            break;
    }
}

/* gfx_font_run_box_rect(), grown by one pixel on every side - see
 * gfx_font_row_run_rect_dilated()'s own comment; the same Minkowski
 * argument holds for any box, not just a single-row run. */
static inline void
gfx_font_run_box_rect_dilated(const gfx_font_t* f, int x, int y, int row0, int row1, int col0, int col1, int scale,
                              int turn, int* out_x, int* out_y, int* out_w, int* out_h) {
    gfx_font_run_box_rect(f, x, y, row0, row1, col0, col1, scale, turn, out_x, out_y, out_w, out_h);
    *out_x -= 1;
    *out_y -= 1;
    *out_w += 2;
    *out_h += 2;
}

/* One coalesced box of a glyph's own set bits - [row0, row1) x [col0,
 * col1], in glyph-local coordinates, before any turn is applied. What
 * gfx_font_glyph_run_boxes() below emits instead of one entry per row. */
typedef struct {
    int row0, row1;
    int col0, col1;
} gfx_font_run_box_t;

/* A caller's own gfx_font_run_box_t[] needs no more than this many slots
 * for the one 1bpp font shipped (gfx_font_8x8, 8 wide): worst case, every
 * row's up to 4 runs fail to match its neighbour, giving cell_h * 4 - 32
 * for an 8-row glyph. */
#define GFX_FONT_RUN_BOXES_MAX 32

/* A 1bpp glyph's own bit-runs, coalesced across consecutive rows sharing
 * the identical [col0, col1] - a vertical stroke becomes one box instead
 * of one per row, before rotation, so the merge is turn-independent. At
 * most cell_w/2 runs open at once (an alternating bit pattern), well
 * under GFX_FONT_MAX_OPEN_RUNS for the one 1bpp font shipped (8 wide).
 * Returns boxes written, capped at `max_out` like dirty_leaf_rects()
 * (gfx_dirty.h); ch outside the font's range yields zero. */
static inline int
gfx_font_glyph_run_boxes(const gfx_font_t* f, unsigned char ch, gfx_font_run_box_t* out, int max_out) {
    if (ch < f->first || (unsigned)(ch - f->first) >= f->count) {
        return 0;
    }

#define GFX_FONT_MAX_OPEN_RUNS 8
    const uint8_t* glyph = f->atlas + (size_t)(ch - f->first) * f->cell_h;
    gfx_font_run_box_t open_runs[GFX_FONT_MAX_OPEN_RUNS];
    int open_count = 0;
    int n = 0;

    /* One extra pass with bits == 0 flushes every run still open once
     * the real rows are done, without a separate closing loop. */
    for (int row = 0; row <= f->cell_h; row++) {
        const uint8_t bits = (row < f->cell_h) ? glyph[row] : 0;

        gfx_font_run_box_t current[GFX_FONT_MAX_OPEN_RUNS];
        int current_count = 0;
        int col = 0;
        while (col < f->cell_w) {
            if (!(bits & (1 << col))) {
                col++;
                continue;
            }
            int end = col;
            while (end + 1 < f->cell_w && (bits & (1 << (end + 1)))) {
                end++;
            }
            if (current_count < GFX_FONT_MAX_OPEN_RUNS) {
                current[current_count].row0 = row;
                current[current_count].row1 = row + 1;
                current[current_count].col0 = col;
                current[current_count].col1 = end;
                current_count++;
            }
            col = end + 1;
        }

        bool current_matched[GFX_FONT_MAX_OPEN_RUNS] = {0};
        gfx_font_run_box_t next_open[GFX_FONT_MAX_OPEN_RUNS];
        int next_open_count = 0;

        /* Extend an open run whose [col0, col1] survives into this row;
         * flush it (it stops here) otherwise. */
        for (int p = 0; p < open_count; p++) {
            int found = -1;
            for (int c = 0; c < current_count; c++) {
                if (!current_matched[c] && current[c].col0 == open_runs[p].col0
                    && current[c].col1 == open_runs[p].col1) {
                    found = c;
                    break;
                }
            }
            if (found >= 0) {
                current_matched[found] = true;
                if (next_open_count < GFX_FONT_MAX_OPEN_RUNS) {
                    next_open[next_open_count] = open_runs[p];
                    next_open[next_open_count].row1 = row + 1;
                    next_open_count++;
                }
            } else if (n < max_out) {
                out[n++] = open_runs[p];
            }
        }
        /* A run this row that matched no open one starts fresh here. */
        for (int c = 0; c < current_count; c++) {
            if (!current_matched[c] && next_open_count < GFX_FONT_MAX_OPEN_RUNS) {
                next_open[next_open_count++] = current[c];
            }
        }

        open_count = next_open_count;
        for (int i = 0; i < open_count; i++) {
            open_runs[i] = next_open[i];
        }
    }

    return n;
#undef GFX_FONT_MAX_OPEN_RUNS
}

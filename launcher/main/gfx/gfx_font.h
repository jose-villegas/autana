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

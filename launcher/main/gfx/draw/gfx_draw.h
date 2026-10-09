/*
 * gfx_draw: the drawing primitives, into the framebuffer (gfx.h) or the
 * band being rendered (gfx_band_run.h), clipped and marked dirty for the
 * present side (gfx_present.h).
 *
 * Colours are given as plain 0xRRGGBB so callers never deal with the panel's
 * byte-swapped RGB565 layout; gfx_rgb() handles that conversion.
 */
#pragma once

#include <stdint.h>

#include "gfx/draw/gfx_color.h"
#include "gfx/draw/gfx_font.h"
#include "gfx/draw/gfx_font_roles.h"

/* Glyphs are 8x8 in the font data, drawn at 2x so they are legible on a
 * 368-wide panel. Text metrics elsewhere must agree with these. */
#define GFX_GLYPH_SCALE 2
#define GFX_CHAR_W      (8 * GFX_GLYPH_SCALE)
#define GFX_CHAR_H      (8 * GFX_GLYPH_SCALE)

/* Convert 0xRRGGBB to the panel's pixel format.
 *
 * GFX_RGB in gfx_color.h does the same thing in a constant expression, which
 * is what lets a colour table live in flash rather than RAM. Both go through
 * one definition, so they cannot drift apart. */
gfx_color_t gfx_rgb(uint32_t rgb);

void gfx_clear(gfx_color_t color);

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);

/* Like gfx_fill_rect(), but at `alpha`'s own apparent coverage (0 nothing,
 * 255 solid, 16 graduated steps between) rather than solid: an ordered
 * (Bayer) dither, the cheapest fake transparency this panel can do since
 * it has no blending anywhere. See gfx_draw.c's own comment above the
 * definition for the dither table and why 255 is guaranteed to be
 * exactly as solid as gfx_fill_rect(). */
void gfx_fill_rect_dither(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha);

/* Like gfx_fill_rect(), but MIXED with the destination via
 * gfx_color_mix() (gfx_color.h) at `alpha` (0 leaves the framebuffer
 * untouched, 255 is pixel-identical to gfx_fill_rect(), everything
 * between is a real per-channel blend). Unlike every other fill here,
 * this one READS the destination pixel first, affordable for
 * text-sized areas, not for full-frame work. See gfx_blit_dither() for why a full-frame
 * composite dithers instead. */
void gfx_fill_rect_blend(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha);

/* Composite a source IMAGE over the framebuffer at alpha's own dithered
 * coverage, gfx_fill_rect_dither()'s sibling for a bitmap instead of a
 * flat colour: a covered pixel becomes the source pixel outright, an
 * uncovered one is left as-is, nothing is ever blended. `src_stride` is
 * pixels per source row, so a window into a larger image just offsets
 * `src` and uses the image's own width as stride. See gfx_draw.c's own
 * comment above the definition for why this is cheap even as a
 * full-frame crossfade. */
void gfx_blit_dither(int x, int y, int w, int h, const gfx_color_t* src, int src_stride, uint8_t alpha);

/* Both clip to the framebuffer, so callers need not bounds-check. */
void gfx_pixel(int x, int y, gfx_color_t color);

/* A line from (x0, y0) to (x1, y1), both endpoints included. Coordinates
 * may be anywhere, on screen or not: a line is shortened to its visible
 * part before anything is drawn, so one running far off the panel costs
 * almost nothing. */
void gfx_line(int x0, int y0, int x1, int y1, gfx_color_t color);

/*
 * Two independent choices (how it composites, and whether it owns its
 * first pixel), so flags on one function rather than a family of
 * "add"/"open" name variants inviting another.
 *
 * No antialiasing flag: Wu-style antialiasing redistributes light within a
 * pixel, while what reads as a lit curve on this panel is a falloff several
 * pixels across. A wide-support filter in the manner of Gupta & Sproull is
 * what would help.
 */

/* Add to what is already in the framebuffer instead of replacing it, so two
 * strokes crossing on a black field make a brighter, mixed colour rather than
 * whichever was drawn second. Costs a read as well as a write per pixel. */
#define GFX_LINE_ADD  (1u << 0)

/* Leave the STARTING pixel undrawn. For chaining segments into a
 * polyline: two segments that meet share a pixel, and under GFX_LINE_ADD
 * a shared pixel is added twice, so a curve drawn as a few hundred short
 * segments comes out beaded, with a brighter dot at every joint. Drawing
 * each segment half-open puts exactly one contribution on every pixel of
 * the chain. Only useful from the second segment onward. */
#define GFX_LINE_OPEN (1u << 1)

void gfx_line_ex(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags);

/* Draws at GFX_GLYPH_SCALE, the size the UI is laid out around. */
void gfx_text(int x, int y, const char* text, gfx_color_t color);

/* Same, at an explicit glyph scale. Scale 1 gives 8x8 glyphs and 46 columns
 * across the panel, which is what makes a dense report like the POST table fit
 * on screen at all. */
void gfx_text_scaled(int x, int y, const char* text, gfx_color_t color, int scale);

/* Same, turned in 90-degree steps: 0 is upright, 1 reads top-to-bottom, 2
 * is upside down, 3 reads bottom-to-top. (x, y) is where the first
 * glyph's cell begins, and the string runs away from it in whichever
 * direction the rotation implies. Exists because "the top of the screen"
 * stops meaning the top edge once the device is turned. */
void gfx_text_turned(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns);

/* Text metrics. Kept here so the UI layer and the renderer cannot disagree.
 * `static inline` over gfx_font.h's pure gfx_font_text_width()/
 * gfx_font_height(): no framebuffer, panel or DMA state behind either, so
 * a caller needing only a metric (an app's own microui screen, say) links
 * no more of gfx than gfx_font_ui()'s own font data already costs. */
static inline int
gfx_text_width(const char* text, int len) {
    return gfx_font_text_width(gfx_font_ui(), text, len, GFX_GLYPH_SCALE);
}

static inline int
gfx_text_height(void) {
    return gfx_font_height(gfx_font_ui(), GFX_GLYPH_SCALE);
}

/* The font every gfx_text*() call above draws with is gfx_font_ui()
 * (gfx/draw/gfx_font_roles.h), the UI/body-text role, not something this file
 * names itself. A caller that wants a specific font, or that wants to name a
 * role directly, asks gfx_font_roles.h for it. */

/* The single font-aware drawing path gfx_text(), gfx_text_scaled() and
 * gfx_text_turned() all delegate to, passing gfx_font_ui(). Same
 * (x, y)-is-the-first-glyph's-cell and turn convention. */
void gfx_text_font(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                   const gfx_font_t* font);

/* gfx_text_font(), but every glyph pixel is drawn through
 * gfx_fill_rect_dither() at `alpha` instead of solid: text that fades
 * rather than cuts. A deliberately separate function, not a parameter
 * added to gfx_text_font() itself; see gfx_draw.c's own comment above the
 * definition for why. */
void gfx_text_font_dither(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                          const gfx_font_t* font, uint8_t alpha);

/* gfx_text_font(), but draws each run one pixel wider on every side
 * instead of its own ink: the halo UI_TEXT_OUTLINED (ui_style.h) casts,
 * in one pass instead of eight unit-offset copies of gfx_text_font()
 * itself. The caller still draws the ink pass afterwards, unchanged. */
void gfx_text_font_halo(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                        const gfx_font_t* font);

/* gfx_text_width()'s general form: the width `text` would draw at in
 * `font`, at `scale`. gfx_text_width() is this called with gfx_font_ui().
 * See gfx_font_text_width() in gfx_font.h for the pure metric this wraps,
 * and its own comment for the `len < 0` contract. */
static inline int
gfx_font_width(const gfx_font_t* font, const char* text, int len, int scale) {
    return gfx_font_text_width(font, text, len, scale);
}

/* Restrict subsequent drawing to a rectangle. microui emits clip commands
 * around every container, and honouring them is what stops a scrolled panel
 * painting over the rest of the screen. */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_clear_clip(void);

/*
 * draw: the drawing primitives, into the framebuffer (gfx.h) or the
 * band being rendered (gfx_band_run.h), clipped and marked dirty through
 * gfx_present.h.
 *
 * Colours are given as plain 0xRRGGBB so callers never deal with the panel's
 * byte-swapped RGB565 layout; RGB565BE() handles that conversion.
 */
#pragma once

#include <stdint.h>

#include "draw/draw_font.h"
#include "draw/draw_font_roles.h"
#include "gfx/target/gfx_color.h"
#include "image/image.h"
#include "image/pixel/rgb565.h"

/* Glyphs are 8x8 in the font data, drawn at 2x so they are legible on a
 * 368-wide panel. Text metrics elsewhere must agree with these. */
#define DRAW_GLYPH_SCALE 2
#define DRAW_CHAR_W      (8 * DRAW_GLYPH_SCALE)
#define DRAW_CHAR_H      (8 * DRAW_GLYPH_SCALE)

void draw_clear(gfx_color_t color);

void draw_rect(int x, int y, int w, int h, gfx_color_t color);

/* Like draw_rect(), but at `alpha`'s own apparent coverage (0 nothing,
 * 255 solid, 16 graduated steps between) rather than solid: an ordered
 * (Bayer) dither, the cheapest fake transparency this panel can do since
 * it has no blending anywhere. See draw.c's own comment above the
 * definition for the dither table and why 255 is guaranteed to be
 * exactly as solid as draw_rect(). */
void draw_rect_dither(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha);

/* Like draw_rect(), but MIXED with the destination via
 * rgb565be_mix() (rgb565.h) at `alpha` (0 leaves the framebuffer
 * untouched, 255 is pixel-identical to draw_rect(), everything
 * between is a real per-channel blend). Unlike every other fill here,
 * this one READS the destination pixel first, affordable for
 * text-sized areas, not for full-frame work. See draw_blit_dither() for why a full-frame
 * composite dithers instead. */
void draw_rect_blend(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha);

/* Composite the whole image at alpha's dithered coverage; reads only IMAGE_RGB565BE.
 * Covered pixels become source pixels; uncovered pixels stay as-is.
 * Use image_sub() for a window into a larger image. */
void draw_blit_dither(int x, int y, const image_t* src, uint8_t alpha);

/* Both clip to the framebuffer, so callers need not bounds-check. */
void draw_pixel(int x, int y, gfx_color_t color);

/* A line from (x0, y0) to (x1, y1), both endpoints included. Coordinates
 * may be anywhere, on screen or not: a line is shortened to its visible
 * part before anything is drawn, so one running far off the panel costs
 * almost nothing. */
void draw_line(int x0, int y0, int x1, int y1, gfx_color_t color);

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
#define DRAW_LINE_ADD          (1u << 0)

/* Leave the STARTING pixel undrawn. For chaining segments into a
 * polyline: two segments that meet share a pixel, and under DRAW_LINE_ADD
 * a shared pixel is added twice, so a curve drawn as a few hundred short
 * segments comes out beaded, with a brighter dot at every joint. Drawing
 * each segment half-open puts exactly one contribution on every pixel of
 * the chain. Only useful from the second segment onward. */
#define DRAW_LINE_OPEN         (1u << 1)

/* Dash lengths are 0..255 pixels, counted from the original line start,
 * including an open first pixel. Both zero means solid. */
#define DRAW_LINE_DASHED       (1u << 2)
#define DRAW_LINE_DASH_ON_BIT  8
#define DRAW_LINE_DASH_OFF_BIT 16
#define DRAW_LINE_DASH_MASK    255u
#define DRAW_LINE_DASH(on, off)                                                                                        \
    (DRAW_LINE_DASHED | (((unsigned)(on) & DRAW_LINE_DASH_MASK) << DRAW_LINE_DASH_ON_BIT)                              \
     | (((unsigned)(off) & DRAW_LINE_DASH_MASK) << DRAW_LINE_DASH_OFF_BIT))

void draw_line_ex(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags);

/* Draws at DRAW_GLYPH_SCALE, the size the UI is laid out around. */
void draw_text(int x, int y, const char* text, gfx_color_t color);

/* Same, at an explicit glyph scale. Scale 1 gives 8x8 glyphs and 46 columns
 * across the panel, which is what makes a dense report like the POST table fit
 * on screen at all. */
void draw_text_scaled(int x, int y, const char* text, gfx_color_t color, int scale);

/* Same, turned in 90-degree steps: 0 is upright, 1 reads top-to-bottom, 2
 * is upside down, 3 reads bottom-to-top. (x, y) is where the first
 * glyph's cell begins, and the string runs away from it in whichever
 * direction the rotation implies. Exists because "the top of the screen"
 * stops meaning the top edge once the device is turned. */
void draw_text_turned(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns);

/* Text metrics. Kept here so the UI layer and the renderer cannot disagree.
 * `static inline` over draw_font.h's pure draw_font_text_width()/
 * draw_font_height(): no framebuffer, panel or DMA state behind either, so
 * a caller needing only a metric (an app's own microui screen, say) links
 * no more of gfx than draw_font_ui()'s own font data already costs. */
static inline int
draw_text_width(const char* text, int len) {
    return draw_font_text_width(draw_font_ui(), text, len, DRAW_GLYPH_SCALE);
}

static inline int
draw_text_height(void) {
    return draw_font_height(draw_font_ui(), DRAW_GLYPH_SCALE);
}

/* The font every draw_text*() call above draws with is draw_font_ui()
 * (draw/draw_font_roles.h), the UI/body-text role, not something this file
 * names itself. A caller that wants a specific font, or that wants to name a
 * role directly, asks draw_font_roles.h for it. */

/* The single font-aware drawing path draw_text(), draw_text_scaled() and
 * draw_text_turned() all delegate to, passing draw_font_ui(). Same
 * (x, y)-is-the-first-glyph's-cell and turn convention. */
void draw_text_font(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                    const draw_font_t* font);

/* draw_text_font() with every glyph pixel dithered at `alpha`: text that
 * fades rather than cuts. */
void draw_text_font_dither(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                           const draw_font_t* font, uint8_t alpha);

/* draw_text_font(), but draws each run one pixel wider on every side
 * instead of its own ink: the halo UI_TEXT_OUTLINED (ui_style.h) casts,
 * in one pass instead of eight unit-offset copies of draw_text_font()
 * itself. The caller still draws the ink pass afterwards, unchanged. */
void draw_text_font_halo(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                         const draw_font_t* font);

/* Restrict subsequent drawing to a rectangle. microui emits clip commands
 * around every container, and honouring them is what stops a scrolled panel
 * painting over the rest of the screen. */
void draw_set_clip(int x, int y, int w, int h);
void draw_clear_clip(void);

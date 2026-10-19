/* UI style geometry. Styles emit commands rather than pixels so command-list
 * hashing sees their changes. Pure spans keep geometry tests independent
 * of drawing and microui functions. */
#pragma once

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

#include "microui.h"
#include "ui/ui.h"

/* 0xRRGGBB, opaque: UI_RGB() as an initializer, so a theme can be const
 * data, and ui_rgb() in an expression. */
#define UI_RGB(rgb)                                                                                                    \
    {(unsigned char)(((rgb) >> UI_RGB_RED_SHIFT) & 0xFF), (unsigned char)(((rgb) >> UI_RGB_GREEN_SHIFT) & 0xFF),       \
     (unsigned char)((rgb) & 0xFF), 255}

/* The shell look, what ui_init() styles and ui_theme(NULL) restores.
 * Deliberately dark: this is an OLED, so black pixels are off pixels; it
 * costs less power and looks better than a grey chrome. It has no panel
 * edge, so a slider there is drawn without a frame. */
#define UI_SHELL_BACKGROUND   0x0A0C14
#define UI_SHELL_TEXT         0xE6EAF2
#define UI_SHELL_BUTTON       0x161A28
#define UI_SHELL_BUTTON_HOVER 0x232A40
#define UI_SHELL_ACCENT       0x3DDC97
#define UI_SHELL_PANEL_FACE   0x10141F
#define UI_SHELL_CAPTION      0x8C96AA
#define UI_SHELL_MUTED        0x4B5266

/* Where red and green sit in an 0xRRGGBB value; blue is the low byte. */
#define UI_RGB_RED_SHIFT      (2 * CHAR_BIT)
#define UI_RGB_GREEN_SHIFT    CHAR_BIT

/* UI_RGB()'s inverse: a colour's 0xRRGGBB, alpha dropped. */
static inline uint32_t
ui_rgb_value(mu_Color c) {
    return ((uint32_t)c.r << UI_RGB_RED_SHIFT) | ((uint32_t)c.g << UI_RGB_GREEN_SHIFT) | c.b;
}

/* One flat rectangle of a styled frame, in paint order: later spans draw
 * over earlier ones, which is what decides how the corners meet. */
typedef struct {
    mu_Rect rect;
    mu_Color color;
} ui_span_t;

/* Face, plus a lit pair of edges and a shadowed pair. */
#define UI_BEZEL_MAX_SPANS 5

/* 3px, for a 64px row (UI_ROW_HEIGHT) at a 2x glyph scale. One pixel is a
 * hairline at this size and vanishes on an OLED at arm's length; much more
 * starts eating the label's padding. */
#define UI_BEZEL_THICKNESS 3

/* How far each edge is mixed toward white and toward black, out of 255.
 * Not symmetric, deliberately: this palette is nearly black by design,
 * so a shadow mixed toward black lands within a shade or two of the
 * window background and disappears: no room below the face colour to
 * carve into. The lit pair carries the whole effect, and WHICH TWO
 * EDGES ARE LIT is the cue that reads as raised or sunken. The shadow
 * is kept anyway because it still separates one button from the next
 * where two rows meet. */
#define UI_BEZEL_HIGHLIGHT 96
#define UI_BEZEL_SHADOW    120

/* Mix a colour toward white (t > 0) or black (t < 0), |t| out of 255.
 * Plain 8-bit channels, unlike gfx_color_mix() in gfx_color.h, which
 * unpacks the panel's byte-swapped RGB565 first: a style works in
 * microui's colour space, never sees a panel pixel; the conversion
 * happens once, in ui.c's draw_command(). Same rounded
 * (a*(255-t) + b*t + 127)/255 mix, so the ends land exactly on the
 * input and target. Alpha is carried through untouched: the edges of a
 * frame are exactly as opaque as its face. */
static inline uint8_t
ui_shade_channel(uint8_t v, int t) {
    const int target = (t >= 0) ? 255 : 0;
    const int amount = (t >= 0) ? t : -t;
    return (uint8_t)((v * (255 - amount) + target * amount + 127) / 255);
}

static inline mu_Color
ui_shade(mu_Color c, int t) {
    return (mu_Color){ui_shade_channel(c.r, t), ui_shade_channel(c.g, t), ui_shade_channel(c.b, t), c.a};
}

/* Shared geometry behind ui_bezel_spans() and ui_panel_spans(): a face rect
 * plus a top/left edge pair and a bottom/right edge pair, back to front,
 * OVERLAPPING at corners, the classic mitre-free bevel; insetting leaves
 * bare corner pixels that read as chipped. Returns spans written, or 0 if
 * `max` can't hold a face plus its four edges. Thickness is clamped so
 * opposite edges never cross; with no room, the result is one flat face
 * span. */
static inline int
ui_frame_spans(mu_Rect r, mu_Color face, int thickness, mu_Color top_left, mu_Color bottom_right, ui_span_t* out,
               int max) {
    if (max < UI_BEZEL_MAX_SPANS || r.w <= 0 || r.h <= 0) {
        return 0;
    }

    out[0] = (ui_span_t){r, face};

    /* Leave at least one pixel of face visible between opposite edges. */
    const int room = (mu_min(r.w, r.h) - 1) / 2;
    const int t = mu_min(thickness, room);
    if (t < 1) {
        return 1;
    }

    out[1] = (ui_span_t){(mu_Rect){r.x, r.y, r.w, t}, top_left};
    out[2] = (ui_span_t){(mu_Rect){r.x, r.y, t, r.h}, top_left};
    out[3] = (ui_span_t){(mu_Rect){r.x, r.y + r.h - t, r.w, t}, bottom_right};
    out[4] = (ui_span_t){(mu_Rect){r.x + r.w - t, r.y, t, r.h}, bottom_right};
    return UI_BEZEL_MAX_SPANS;
}

/* `sunken` swaps which edge pair is lit, raised into pressed; see
 * ui_frame_spans() for the shared geometry and its all-or-nothing rule,
 * same as UI_BUTTON_FLAT falls back to when there is no room for edges. */
static inline int
ui_bezel_spans(mu_Rect r, mu_Color face, bool sunken, ui_span_t* out, int max) {
    const mu_Color lit = ui_shade(face, UI_BEZEL_HIGHLIGHT);
    const mu_Color shadow = ui_shade(face, -UI_BEZEL_SHADOW);
    return ui_frame_spans(r, face, UI_BEZEL_THICKNESS, sunken ? shadow : lit, sunken ? lit : shadow, out, max);
}

/*
 * Text
 *
 * A second style, sibling to the bezel above, for exactly the same reason:
 * WHAT to draw (a string, at this position, in this colour) is microui's
 * question, and HOW it reads against whatever it sits on is a looks question
 * that should not require touching a call site. See suite_ui_style.c for the
 * geometry checks.
 */

/* One drawing pass of a styled string, in paint order. */
typedef struct {
    int dx, dy;
    bool ink;
} ui_text_pass_t;

/* PLAIN is 1, SHADOWED is 2, OUTLINED is 9 (8 halo offsets + the ink); the
 * largest of the three sizes the buffer for all of them. */
#define UI_TEXT_MAX_PASSES 9

/* The passes making up one styled string, back to front. Returns how
 * many were written, or 0 if `max` cannot hold them all, the same
 * all-or-nothing rule ui_bezel_spans() follows, for the same reason: a
 * half-drawn outline looks like a bug. THE INK PASS MUST ALWAYS BE
 * LAST. Every other pass paints the halo, which has to sit *behind* the
 * glyph it is haloing: draw the halo first and the glyph on top, or
 * the glyph disappears under its own halo. */
static inline int
ui_text_passes(ui_text_style_t style, ui_text_pass_t* out, int max) {
    switch (style) {
        case UI_TEXT_PLAIN:
            if (max < 1) {
                return 0;
            }
            out[0] = (ui_text_pass_t){0, 0, true};
            return 1;

        case UI_TEXT_SHADOWED:
            if (max < 2) {
                return 0;
            }
            out[0] = (ui_text_pass_t){1, 1, false};
            out[1] = (ui_text_pass_t){0, 0, true};
            return 2;

        case UI_TEXT_OUTLINED: {
            if (max < UI_TEXT_MAX_PASSES) {
                return 0;
            }
            /* One pixel each way, in screen space. This mirrors an app's own
         * hand-rolled label-outline code exactly, including the order:
         * that code is the precedent this style generalises, and it is
         * worth staying a recognisably identical list rather than an
         * equivalent but different-looking one. All eight, not just the
         * four cardinals: at DRAW_GLYPH_SCALE 2 each
         * font pixel is a 2x2 block, so skipping the diagonals leaves a
         * notch at every block corner rather than a clean edge. */
            static const int offsets[8][2] = {
                {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1},
            };
            for (int i = 0; i < 8; i++) {
                out[i] = (ui_text_pass_t){offsets[i][0], offsets[i][1], false};
            }
            out[8] = (ui_text_pass_t){0, 0, true};
            return UI_TEXT_MAX_PASSES;
        }

        default: return 0;
    }
}

/* A section frame for a screen's captioned groups: a face plus a plain
 * border, sibling to the bezel above but flat rather than lit/shadowed; a
 * panel groups content, it does not invite a press. */

/* Face, plus four border edges. */
#define UI_PANEL_MAX_SPANS        5

/* 2px: thinner than UI_BEZEL_THICKNESS on purpose; a panel outlines a
 * whole screen section, not a single tap target, so a hairline reads as a
 * grouping without competing with the bezelled controls inside it. */
#define UI_PANEL_BORDER_THICKNESS 2

/* One border colour on every edge, unlike the bezel's lit/shadowed pair;
 * see ui_frame_spans() for the shared geometry and its all-or-nothing
 * rule. */
static inline int
ui_panel_spans(mu_Rect r, mu_Color face, mu_Color border, ui_span_t* out, int max) {
    return ui_frame_spans(r, face, UI_PANEL_BORDER_THICKNESS, border, border, out, max);
}

/* The halo colour for a given ink, derived from the ink's luminance rather
 * than fixed; see UI_BEZEL_HIGHLIGHT/SHADOW above. A fixed halo vanishes
 * against whichever ink matches it. A badge sitting on a known pair can pick
 * the contrasting one; a general halo has no such pair, so it goes to the
 * opposite extreme via ui_shade(); a partial mix can still wash out. */
static inline mu_Color
ui_text_halo(mu_Color ink) {
    /* Same weights as a standard perceptual luma (~0.30/0.59/0.11 scaled to
     * whole numbers as 2:5:1), just enough to tell a dark ink from a light
     * one; it does not need to be exact, only decisive. Range is
     * 0..255*8 = 0..2040; 1020 is the midpoint. */
    const int luma = ink.r * 2 + ink.g * 5 + ink.b;
    return ui_shade(ink, (luma >= 1020) ? -255 : 255);
}

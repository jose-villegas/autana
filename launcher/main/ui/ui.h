/*
 * ui: the plain-value UI face, for the shell and for apps. The library
 * behind it is private to ui/.
 *
 * Everything an app needs to draw a UI, and nothing about any particular UI.
 * The launcher is one caller; an app wanting a settings panel or a readout is
 * another, and gets the same touch handling and the same redraw skipping for
 * free.
 *
 * The face is immediate mode and does no drawing itself: each frame a screen
 * describes its UI again, the frame becomes a list of rectangles, text and
 * icons, and ui_end() walks that list painting into the shared framebuffer.
 * A retained-mode toolkit wants to own the display and the refresh cycle,
 * which fights an app that owns its own drawing. Here a list of primitives is
 * rendered whenever and into whatever the caller likes.
 *
 * The UI is only *rebuilt* every frame, not necessarily *changed*. The list
 * is a complete description of the output, so if it hashes the same as last
 * frame the picture is identical and both the repaint and the transfer are
 * skipped, and a static menu costs nothing at all. Repainting every frame
 * instead would mark every band dirty and force a full-frame transfer in any
 * app that drew a UI.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "draw/draw_font.h"
#include "draw/icon.h"
#include "input/input.h"
#include "math/geometry/recti.h"
#include "ui/layout/ui_transform.h"

/* Shared metrics, so the shell and any app UI look like one product. */
#define UI_TITLE_HEIGHT    56
#define UI_ROW_HEIGHT      64
#define UI_ROW_GAP         8
/* Space above and below a line of text in its row: ui_row(ui_text_height() + UI_TEXT_PAD). */
#define UI_TEXT_PAD        4
#define UI_MARGIN          16
/* A window's inset from its edge to its first row. */
#define UI_PADDING         12

/* Tap targets, in the smaller dimension. The panel is ~322 px per inch
 * (368x448 on a 1.8" diagonal), so a phone's 44 is only 3.5 mm here: 56 is
 * ~4.4 mm, the floor a layout test holds a control to, and 64 (~5 mm) is
 * what a new control should aim for. */
#define UI_TAP_MIN         56
#define UI_TAP_RECOMMENDED 64

/* ui_slider_int()'s knob width: chunky enough for a finger, not tuned
 * finer than that until Phase 5 puts a screenshot next to the design. */
#define UI_SLIDER_KNOB_W   40

/* The strip across the top of the home screen, reserved and deliberately
 * empty. It is where status belongs (battery, connection, the clock),
 * and holding the space open now means adding any of that later moves
 * nothing below it. A row of the menu was there before; a status bar and
 * a heading that only ever said "APPS" cannot both have the top of a
 * 448px screen. */
#define UI_BANNER_HEIGHT   56

/* The page background: what a screen passes to ui_end(), what the shell look
 * paints its windows with, and what an app matches when it draws beside the
 * UI. The one spelling of this colour. */
#define UI_BACKGROUND      0x0A0C14

/* Pass as ui_end()'s background to draw without clearing first, for a UI laid
 * over an app's own output rather than replacing it. */
#define UI_NO_BACKGROUND   0xFFFFFFFFu

/* How text is drawn: one pass, or a halo behind the ink. */
typedef enum {
    UI_TEXT_PLAIN = 0, /* one pass, exactly as today */
    UI_TEXT_OUTLINED,  /* a halo at all eight neighbouring offsets */
    UI_TEXT_SHADOWED,  /* a single offset halo, down-right */
} ui_text_style_t;

void ui_init(void);

/* Start a UI frame: touch becomes the controls' pointer events. A NULL
 * `input` opens a repaint-only boot frame: no input is fed and no pointer
 * state is reported. Use it before input starts, with no touch in progress. */
void ui_begin(const input_t* input);

/* Choose how text is drawn for the rest of this frame. This is not reset by ui_begin() and is not part
 * of the frame's description in the sense that matters for the repaint
 * hash; see the comment above ui_set_text_style()'s definition in ui.c
 * for why a text style needs its own invalidation. */
void ui_set_text_style(ui_text_style_t style);

/* Choose the font text is measured and drawn with, until
 * called again; ui_init() seeds it with draw_font_ui(). NULL falls back
 * to draw_font_ui() rather than being stored. Unlike ui_set_text_style()
 * and ui_set_transform() below, this does NOT need ui_invalidate(); see
 * ui_set_font()'s ui.c comment for why. Equivalent to
 * ui_set_font_scaled(font, DRAW_GLYPH_SCALE). */
void ui_set_font(const draw_font_t* font);

/* Like ui_set_font(), but at `scale` glyph cells instead of the fixed
 * DRAW_GLYPH_SCALE; see ui_set_font()'s ui.c comment for why carrying the
 * scale inside the font, rather than a separate render-time setting, is
 * what lets a screen mix two text sizes without paying ui_invalidate()
 * every frame. Clamped to at least 1. */
void ui_set_font_scaled(const draw_font_t* font, int scale);

/* The width `str` would measure at the CURRENT font and scale: what
 * ui_set_font()/ui_set_font_scaled() last set. For right-aligning a
 * string (e.g. against a caption on the same row) without re-deriving
 * the font role and scale at the call site. */
int ui_measure_text(const char* str);

/* Choose the transform every command is mapped through before it is
 * drawn; see ui_transform.h for what a transform is and why it is
 * fixed point. Identity until called; ui_init() sets it explicitly. Like
 * ui_set_text_style(), this must call
 * ui_invalidate() when the transform changes: it applies at render time
 * inside draw_command(), so the command list hash_canvas() sees is
 * byte-identical regardless; see ui_set_text_style()'s ui.c comment for
 * the full argument. */

/* ONE TRANSFORM PER RENDERED PASS, NOT A STACK. The frame's command list has
 * no way to carry a transform change partway through it, and the vendored library
 * is deliberately unpatched, so there is no per-widget nesting. */
void ui_set_transform(affine2x_t t);

/* Increments whenever the canvas shape genuinely changes. THIS IS NOT A
 * CALLBACK, NOBODY SUBSCRIBES TO IT: a caller reads it, remembers it and
 * compares a fresh read later. Resist a subscriber list: the value comes
 * from being pulled and diffed on the caller's own schedule. */
uint32_t ui_layout_generation(void);

/* The logical canvas size: the physical viewport (GFX_WIDTH x GFX_HEIGHT)
 * mapped through the inverse of the current transform. Callers building
 * a UI must ask these instead of assuming GFX_WIDTH/GFX_HEIGHT directly:
 * under a quarter-turn transform the two swap, and code that measured
 * against the physical panel would lay out past the edge of the rotated
 * canvas or leave a gap at it. */
int ui_width(void);
int ui_height(void);

/* Close the frame and paint it, only if it would look different from the
 * screen; returns whether it drew. It repaints when the UI changed,
 * something else dirtied its pixels, or ui_invalidate() was called.
 * `background_rgb` is cleared to first, or UI_NO_BACKGROUND paints over
 * what is there. With no framebuffer to paint (band mode, or an expanded
 * frame) it bins as ui_end_for_bands() does and returns true. */
bool ui_end(uint32_t background_rgb);

/* ui_end() over a backdrop that is drawn, not a colour. `paint_backdrop`
 * paints the whole screen and is called only when the UI itself changed; a
 * backdrop that animates draws its own changes before this call, and the
 * unchanged UI is put back over them. Full framebuffer only. */
typedef void (*ui_backdrop_fn)(void);
bool ui_end_over(ui_backdrop_fn paint_backdrop);

/* Declare that the framebuffer no longer holds this UI's output, so the
 * next ui_end() must repaint even if the UI is unchanged. Needed
 * whenever something has replaced the screen without going through gfx
 * in a way ui_end() can detect: in practice, returning to the launcher
 * after an app has been running. Without it the launcher would compare
 * its unchanged command list, skip the repaint, and leave the app's
 * last frame on screen. */
void ui_invalidate(void);

/*
 * Closes the frame like ui_end() does, but bins commands by row range
 * instead of painting, whatever the layout; ui_replay_band() draws a band's
 * share when gfx_band_run() draws that band. ui_end() does this itself in
 * band mode. Pass UI_NO_BACKGROUND if the caller already cleared the band.
 */
void ui_end_for_bands(uint32_t background_rgb);

/* Discards the previous frame's replay and queued overlay rectangles. */
void ui_clear_band_overlay(void);

/* Draws whatever ui_end_for_bands() bin holds that overlaps [row0, row1):
 * call once per band, into gfx's current band draw target, after the
 * app's own band content. Commands replay in the same back-to-front,
 * within-canvas order ui_end() itself would paint them in. */
void ui_replay_band(int row0, int row1);

/* Full band replays in the current frame; reset by ui_end_for_bands(). */
uint32_t ui_band_fallback_count(void);

/* Queues a plain opaque rect for the NEXT ui_end_for_bands() call to bin
 * alongside the frame's own commands, for pixels to show in band mode
 * that were never built through the face, such as the shell's own
 * home-swipe hint. Drained (and cleared) by that call, so state it every
 * frame it is wanted. `rgb` is plain 0xRRGGBB, not a gfx_color_t; ui.h
 * does not otherwise depend on gfx.h. */
void ui_queue_band_overlay_rect(int x, int y, int w, int h, uint32_t rgb);

#define UI_FACE_SCOPE_DEPTH 4

/* Colours are 0xRRGGBB. */
typedef struct {
    uint32_t panel_face, panel_edge, button_face;
    uint32_t accent, accent_face, on_accent;
    uint32_t text, caption, muted;
    int text_scale;
    int icon_side;
} ui_theme_t;

/* The palette for the rest of the frame; NULL restores the shell look. */
void ui_theme(const ui_theme_t* theme);

/* A rect `w` wide and `h` tall at `y`, centred in a canvas `canvas_w` wide:
 * fixed-width content, where ui_row() spans the canvas. `w` is not clamped;
 * one wider than the canvas gets a negative x. */
static inline recti_t
ui_centered_rect(int canvas_w, int w, int h, int y) {
    return (recti_t){(canvas_w - w) / 2, y, w, h};
}

/* A label is both the visible text and the identity: "Text##key" shows Text
 * and the whole string is the id; "Text###key" shows Text and only key is the
 * id. ui_button and ui_tile return true on the frame a tap is released on
 * them; ui_toggle, ui_slider and ui_dropdown return true when this frame
 * changed their value. Every _begin pairs with its _end, at most
 * UI_FACE_SCOPE_DEPTH deep; development builds assert on misuse. Windows
 * close at the next window verb or ui_end(). */
void ui_screen(void);
void ui_window(recti_t r);
/* A window whose flowed rows scroll vertically once they overflow `r`. */
void ui_scroll_window(recti_t r);
/* Flow: ui_next() hands out the rows the window's width wide, or `w` wide and
 * centred after ui_row_centre(), each row below the last. */
void ui_row(int h);
void ui_row_centre(int w, int h);
recti_t ui_next(void);

bool ui_button(recti_t r, const char* label);
/* The icon at the left, the label centred in the rest; a NULL icon is
 * ui_button(). */
bool ui_button_icon(recti_t r, const char* label, const draw_image_t* icon);
bool ui_tile(recti_t r, const char* label, bool selected);
/* The icon above the label, in ui_tile_icon_rect(r); a NULL icon is
 * ui_tile(). */
bool ui_tile_icon(recti_t r, const char* label, const draw_image_t* icon, bool selected);
/* The square a tile `r` gives its icon, for a caller drawing its own
 * picture of the choice instead. */
recti_t ui_tile_icon_rect(recti_t r);
bool ui_toggle(recti_t r, const char* label, bool* on);
bool ui_slider(recti_t r, const char* label, int* value, int lo, int hi);
bool ui_dropdown(recti_t r, const char* label, int* index, const char* const* names, int count);
/* How many frames a dropdown's list stays up after its pick, once the
 * finger lifts. */
#define UI_DROPDOWN_CLOSE_FRAMES 4
bool ui_dropdown_icons(recti_t r, const char* label, int* index, const char* const* names,
                       const draw_image_t* const* icons, int count);

/* One line's height at the current theme's text scale. */
int ui_text_height(void);
/* Text is clipped to its rectangle and centred vertically. */
void ui_text(recti_t r, const char* s);
void ui_text_right(recti_t r, const char* s);
void ui_text_centre(recti_t r, const char* s);
void ui_caption(recti_t r, const char* s);
void ui_caption_centre(recti_t r, const char* s);
void ui_header(recti_t r, const char* title);
void ui_panel(recti_t r);
void ui_fill(recti_t r, uint32_t rgb);
void ui_outline(recti_t r, uint32_t rgb);
/* The lit and shaded edges of a button's bezel round `r`, shaded off
 * `face_rgb`; the inside is left as drawn. `sunken` swaps the pair. */
void ui_bevel(recti_t r, uint32_t face_rgb, bool sunken);
/* `icon` filling `r` in the text colour; a NULL icon (no pack holds it)
 * draws `fallback` centred instead, or nothing for a NULL `fallback`. */
void ui_icon(recti_t r, const draw_image_t* icon, const char* fallback);
/* The system check mark filling `r` in the text colour: one recorded mark,
 * painted from the engine pack, however detailed the icon. */
void ui_check(recti_t r);
void ui_swatches(recti_t r, const uint32_t* rgb, int cols, int rows);

void ui_disabled_begin(bool disabled);
void ui_disabled_end(void);

typedef enum { UI_COLOR_FACE, UI_COLOR_TEXT } ui_color_slot_t;

void ui_color_begin(ui_color_slot_t slot, uint32_t rgb);
void ui_color_end(void);
void ui_scale_begin(int scale);
void ui_scale_end(void);
/* Text verbs inside draw their ink at `alpha`/255 coverage, dithered and
 * without the outline halo; 0 draws no text, 255 is opaque. */
void ui_alpha_begin(uint8_t alpha);
void ui_alpha_end(void);

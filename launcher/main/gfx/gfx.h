/*
 * gfx - framebuffer ownership and drawing primitives.
 *
 * Everything on this device draws into ONE full-screen RGB565 framebuffer that
 * this module owns. The shell and every app share it; nothing else allocates a
 * buffer of its own. At 368x448x2 that single buffer is 322 KiB of the ~424
 * KiB the chip has, so a second one is not affordable.
 *
 * Colours are given as plain 0xRRGGBB so callers never deal with the panel's
 * byte-swapped RGB565 layout - gfx_rgb() handles that conversion.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "bsp/esp-bsp.h"
#endif
#include "gfx/gfx_band.h"
#include "gfx/gfx_color.h"
#include "gfx/gfx_fb_guard.h"
#include "gfx/gfx_font.h"
#include "gfx/gfx_indexed.h"
#include "gfx/gfx_mode.h"

/* ESP_PLATFORM is defined by ESP-IDF's own toolchain file - never by this
 * project - which is what makes it the natural, zero-plumbing switch
 * between the device build and a host one: a plain `gcc` invocation (the
 * same one test/run_tests.sh already uses) simply never defines it. The
 * host figures are literals rather than pulled from anywhere, the same
 * reason gfx_dirty.h already hardcodes them - a BSP header is exactly
 * what a host build cannot include. */
#ifdef ESP_PLATFORM
#define GFX_WIDTH  BSP_LCD_H_RES /* 368 */
#define GFX_HEIGHT BSP_LCD_V_RES /* 448 */
#else
#define GFX_WIDTH  368
#define GFX_HEIGHT 448
#endif

/* QSPI clock for the panel - the largest single cost in a frame: 16.5 ms of
 * bus at 40 MHz against 8.2 at 80. 80 is the default and holds only while
 * every strip is sent from internal DMA RAM (gfx.c, strip_bounce); see
 * CONFIG_LAUNCHER_GFX_QSPI_80MHZ. The divider resolves to exactly 40 or 80,
 * hence a bool. THE THRESHOLDS BELOW ARE FITTED TO 40 MHz. */
#if defined(CONFIG_LAUNCHER_GFX_QSPI_80MHZ) && CONFIG_LAUNCHER_GFX_QSPI_80MHZ
#define GFX_QSPI_HZ (80 * 1000 * 1000)
#else
#define GFX_QSPI_HZ (40 * 1000 * 1000)
#endif

/* The band ring's compile-time band height (gfx_mode.h, gfx_band.h) - a
 * divisor of GFX_HEIGHT (448): 64, 32 or 16. 32 is the default absent a
 * device sweep saying otherwise (docs/Autana-Rendering-Roadmap.md section
 * 8, decision 2); override with -DGFX_BAND_HEIGHT=N to try another. */
#ifndef GFX_BAND_HEIGHT
#if defined(CONFIG_LAUNCHER_GFX_BAND_HEIGHT_16) && CONFIG_LAUNCHER_GFX_BAND_HEIGHT_16
#define GFX_BAND_HEIGHT 16
#elif defined(CONFIG_LAUNCHER_GFX_BAND_HEIGHT_64) && CONFIG_LAUNCHER_GFX_BAND_HEIGHT_64
#define GFX_BAND_HEIGHT 64
#else
#define GFX_BAND_HEIGHT 32
#endif
#endif
_Static_assert(GFX_HEIGHT % GFX_BAND_HEIGHT == 0, "GFX_BAND_HEIGHT must divide GFX_HEIGHT evenly");
_Static_assert(GFX_BAND_HEIGHT % 2 == 0, "a band's row range must round to even panel window edges");

/* Glyphs are 8x8 in the font data, drawn at 2x so they are legible on a
 * 368-wide panel. Text metrics elsewhere must agree with these. */
#define GFX_GLYPH_SCALE 2
#define GFX_CHAR_W      (8 * GFX_GLYPH_SCALE)
#define GFX_CHAR_H      (8 * GFX_GLYPH_SCALE)

/* Brings up the panel and allocates the framebuffer.
 * Returns false if either fails; the reason is logged. */
bool gfx_init(void);

/* Convert 0xRRGGBB to the panel's pixel format.
 *
 * GFX_RGB in gfx_color.h does the same thing in a constant expression, which
 * is what lets a colour table live in flash rather than RAM. Both go through
 * one definition, so they cannot drift apart. */
gfx_color_t gfx_rgb(uint32_t rgb);

/* Direct access, for renderers that write pixels in bulk (the 3D rasterizer
 * writes here directly rather than going through gfx_pixel per fragment). */
gfx_color_t* gfx_framebuffer(void);

void gfx_clear(gfx_color_t color);

/* Enable or disable partial clear mode. When enabled, gfx_clear() erases only
 * the bounding box of what was marked dirty on the previous frame instead of
 * wiping the entire 322 KiB framebuffer, and automatically marks that erased
 * region dirty for presentation. Off by default. */
void gfx_set_partial_clear(bool enabled);
bool gfx_partial_clear_enabled(void);

/* Enable or disable interlace mode. When enabled, gfx_present() updates
 * only even-numbered strips on even frames and odd-numbered strips on odd
 * frames. Off by default. */
void gfx_set_interlace(bool enabled);
bool gfx_interlace_enabled(void);

/* Forces the next gfx_clear() to wipe the entire screen in full, resetting
 * partial clear tracking. Needed when an app opens, closes, or rotates. */
void gfx_invalidate(void);

/* One call for a transition instead of composing gfx_mark_all_dirty() and
 * gfx_invalidate() separately, plus a pending latch (below) an app's
 * optional invalidate() callback (app.h) answers to. Sets state only and
 * frees nothing, so it is safe from anywhere on core 0, an app callback or
 * a UI build included. */
void gfx_request_full_redraw(void);

/* True from a gfx_request_full_redraw() call until the shell clears it for
 * the pass that follows. */
bool gfx_full_redraw_pending(void);

/* Ends the window gfx_request_full_redraw() opened - called by the shell
 * once it has read the flag and decided whether to invoke an app's
 * invalidate(), before that pass's frame() runs. */
void gfx_full_redraw_clear_pending(void);

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);

/* Like gfx_fill_rect(), but at `alpha`'s own apparent coverage (0 nothing,
 * 255 solid, 16 graduated steps between) rather than solid - an ordered
 * (Bayer) dither, the cheapest fake transparency this panel can do since
 * it has no blending anywhere. See gfx.c's own comment above the
 * definition for the dither table and why 255 is guaranteed to be
 * exactly as solid as gfx_fill_rect(). */
void gfx_fill_rect_dither(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha);

/* Like gfx_fill_rect(), but MIXED with the destination via
 * gfx_color_mix() (gfx_color.h) at `alpha` (0 leaves the framebuffer
 * untouched, 255 is pixel-identical to gfx_fill_rect(), everything
 * between is a real per-channel blend). Unlike every other fill here,
 * this one READS the destination pixel first - affordable for
 * text-sized areas (this exists for an 8bpp coverage-atlas font), NOT
 * for full-frame work - see gfx_blit_dither() for why a full-frame
 * composite dithers instead. */
void gfx_fill_rect_blend(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha);

/* Composite a source IMAGE over the framebuffer at alpha's own dithered
 * coverage - gfx_fill_rect_dither()'s sibling for a bitmap instead of a
 * flat colour: a covered pixel becomes the source pixel outright, an
 * uncovered one is left as-is, nothing is ever blended. `src_stride` is
 * pixels per source row, so a window into a larger image just offsets
 * `src` and uses the image's own width as stride. See gfx.c's own
 * comment above the definition for why this is cheap even as a
 * full-frame crossfade. */
void gfx_blit_dither(int x, int y, int w, int h, const gfx_color_t* src, int src_stride, uint8_t alpha);

/* Both clip to the framebuffer, so callers need not bounds-check. */
void gfx_pixel(int x, int y, gfx_color_t color);

/* A line from (x0, y0) to (x1, y1), both endpoints included. Coordinates
 * may be anywhere, on screen or not: a line is shortened to its visible
 * part before anything is drawn, so one running far off the panel costs
 * almost nothing. Exists because the startup animation plots a curve, and
 * a curve is a few hundred short segments - see boot_anim.c. Nothing
 * before it needed a line at all, which is why this is the newest
 * primitive in the file. */
void gfx_line(int x0, int y0, int x1, int y1, gfx_color_t color);

/*
 * Two independent choices - how it composites, and whether it owns its
 * first pixel - so flags on one function rather than a family of
 * gfx_line_add_open() spellings inviting another.
 *
 * A GFX_LINE_SMOOTH doing Xiaolin Wu antialiasing cost 6.7 fps to be nearly
 * invisible: antialiasing redistributes light WITHIN a pixel, while what
 * reads as a lit curve on this panel is a falloff several pixels ACROSS.
 * The thing to come back with is a wide-support filter in the manner of
 * Gupta & Sproull, not that.
 */

/* Add to what is already in the framebuffer instead of replacing it, so two
 * strokes crossing on a black field make a brighter, mixed colour rather than
 * whichever was drawn second. Costs a read as well as a write per pixel. */
#define GFX_LINE_ADD  (1u << 0)

/* Leave the STARTING pixel undrawn. For chaining segments into a
 * polyline: two segments that meet share a pixel, and under GFX_LINE_ADD
 * a shared pixel is added twice - so a curve drawn as a few hundred short
 * segments comes out beaded, with a brighter dot at every joint. Drawing
 * each segment half-open puts exactly one contribution on every pixel of
 * the chain. Only useful from the second segment onward. */
#define GFX_LINE_OPEN (1u << 1)

void gfx_line_ex(int x0, int y0, int x1, int y1, gfx_color_t color, unsigned flags);

/* Draws at GFX_GLYPH_SCALE - the size the UI is laid out around. */
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

/* Text metrics. Kept here so the UI layer and the renderer cannot disagree. */
int gfx_text_width(const char* text, int len);
int gfx_text_height(void);

/* The font every gfx_text*() call above draws with is gfx_font_ui()
 * (gfx/gfx_font_roles.h) - the UI/body-text role, not something this file
 * names itself any more (it used to, as gfx_default_font()). A caller
 * that wants a specific font, or that wants to name a role directly, asks
 * gfx_font_roles.h for it. */

/* The single font-aware drawing path gfx_text(), gfx_text_scaled() and
 * gfx_text_turned() all delegate to, passing gfx_font_ui(). Same
 * (x, y)-is-the-first-glyph's-cell and turn convention. Draws bpp==1
 * (1-bit mask) and bpp==8 (8-bit coverage atlas) glyphs; anything else is
 * silently skipped rather than drawn wrong - see draw_glyph_font()'s own
 * comment in gfx.c for why those are the only two layouts with a defined
 * meaning. */
void gfx_text_font(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                   const gfx_font_t* font);

/* gfx_text_font(), but every glyph pixel is drawn through
 * gfx_fill_rect_dither() at `alpha` instead of solid - text that fades
 * rather than cuts. A deliberately separate function, not a parameter
 * added to gfx_text_font() itself - see gfx.c's own comment above the
 * definition for why. */
void gfx_text_font_dither(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                          const gfx_font_t* font, uint8_t alpha);

/* gfx_text_font(), but draws each run one pixel wider on every side
 * instead of its own ink - the halo UI_TEXT_OUTLINED (ui_style.h) casts,
 * in one pass instead of eight unit-offset copies of gfx_text_font()
 * itself. Only bpp==1 fonts (gfx_font_row_run_rect_dilated()'s own
 * comment): the caller still draws the ink pass afterwards, unchanged. */
void gfx_text_font_halo(int x, int y, const char* text, gfx_color_t color, int scale, int quarter_turns,
                        const gfx_font_t* font);

/* gfx_text_width()'s general form: the width `text` would draw at in
 * `font`, at `scale`. gfx_text_width() is this called with gfx_font_ui().
 * See gfx_font_text_width() in gfx_font.h for the pure metric this wraps,
 * and its own comment for the `len < 0` contract. */
int gfx_font_width(const gfx_font_t* font, const char* text, int len, int scale);

/* Restrict subsequent drawing to a rectangle. microui emits clip commands
 * around every container, and honouring them is what stops a scrolled panel
 * painting over the rest of the screen. */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_clear_clip(void);

/*
 * gfx_present() sends only the horizontal bands that changed - the panel
 * holds the rest in its own GRAM, and sending is almost the whole cost of a
 * frame. Every gfx_* drawing call marks what it touched and gfx_clear()
 * marks the whole screen, so most callers never touch this. Code writing
 * through gfx_framebuffer() directly MUST mark what it wrote; forgetting
 * looks like a frozen or partially stale screen, not a crash.
 */

/* Declare that a rectangle of the framebuffer has changed. Tracked as a real
 * box per grid cell, not just which cell - a caller that knows it only
 * touched part of a cell may end up sending less than the whole thing. */
void gfx_mark_dirty(int x, int y, int w, int h);

void gfx_mark_all_dirty(void);

/* Whether any band overlapping this rectangle is already going to be
 * sent. For overlay content that is identical every frame - the shell's
 * home hint - this answers "does it need redrawing?". If nothing below
 * it changed, the pixels are still in the framebuffer and still on the
 * panel, and both the draw and the transfer can be skipped. */
bool gfx_region_dirty(int x, int y, int w, int h);

/* Send the changed bands to the panel and wait for the transfers to land.
 * The wait is mandatory - see the notes on asynchronous DMA in the docs.
 * Exactly gfx_present_begin() followed by gfx_present_wait(); every existing
 * caller keeps working unchanged under the core-1 present task below. */
void gfx_present(void);

/*
 * Split present: gfx_present_begin() hands the framebuffer to the core-1
 * present task and returns at once; gfx_present_wait() blocks until sent.
 * Between the two, no gfx_* call that touches drawing state or the
 * framebuffer may run - app.h's update() contract, asserted in development
 * builds (gfx_present_guard.h). gfx_set_present_async(false) sends
 * synchronously on the caller instead, for A/B measurement.
 */
void gfx_present_begin(void);
void gfx_present_wait(void);

void gfx_set_present_async(bool on);
bool gfx_present_async_enabled(void);

/* The panel link's clock. The fast rate halves bus time but is past the
 * panel's rated 50 MHz: a region can land with stray pixels that stay until
 * it is sent again. Saved on the device; GFX_QSPI_HZ is the first-boot
 * default. */
#define GFX_PANEL_CLOCK_SLOW_HZ (40 * 1000 * 1000)
#define GFX_PANEL_CLOCK_FAST_HZ (80 * 1000 * 1000)

/* Takes effect before the next present sends anything, never mid-send.
 * Returns false, changing nothing, for any other rate. */
bool gfx_set_panel_clock_hz(int hz);
int gfx_panel_clock_hz(void);

/*
 * Heal, an opt-in for an app that sends only what changed: gfx sends a
 * marked region again on a later present, as full-width strips cut
 * differently from any earlier send, to clear what the fast clock left
 * wrong. All of it does nothing while the clock is the slow one.
 */

#define GFX_HEAL_DEFAULT_BUDGET_PIXELS (GFX_WIDTH * 32)

/* Queues rows [y, y + h) for healing; x and w are ignored, strips are full
 * width. Safe wherever gfx_mark_dirty() is. */
void gfx_heal_mark(int x, int y, int w, int h);

/* How many pixels of heal one present may add. */
void gfx_heal_set_budget(int pixels_per_present);

/* Rows per present of a sweep over the whole screen, 0 for none - for an app
 * that wants healing without a policy. Whoever turns it on turns it off. */
void gfx_heal_set_rolling(int rows_per_present);

bool gfx_heal_active(void);

/*
 * Mode: a full PSRAM framebuffer, or an internal-SRAM band ring for a
 * full-redraw renderer (docs/Autana-Rendering-Roadmap.md section 3.3).
 * Requested from enter(), released with gfx_mode_exit() from exit(). Only
 * full resolution with no interlace renders; other requests grant
 * correctly (gfx_mode.h) but nothing consumes them yet.
 */

/* Grants `request`, allocates whatever the granted layout needs, and
 * returns the grant. Asserts the current mode is already GFX_LAYOUT_FULL_FB:
 * nesting one app's mode inside another's is not supported. */
const gfx_mode_t* gfx_mode_enter(const gfx_mode_request_t* request);

/* Frees whatever the current mode allocated and restores GFX_LAYOUT_FULL_FB
 * at full resolution, no interlace - the mode every app but the one just
 * exiting assumes is already in force. */
void gfx_mode_exit(void);

const gfx_mode_t* gfx_mode_current(void);

/*
 * The band ring, valid only while gfx_mode_current()->layout is
 * GFX_LAYOUT_BANDS:
 *
 *     gfx_band_frame_begin();
 *     while (gfx_band_next()) {
 *         ...draw into gfx_band_buffer(), rows gfx_band_row0().. ...
 *         gfx_band_submit();
 *     }
 *
 * gfx_band_next() returning false has already waited for the last band's
 * send to land.
 */
void gfx_band_frame_begin(void);
bool gfx_band_next(void);
gfx_color_t* gfx_band_buffer(void);
int gfx_band_row0(void);
int gfx_band_height(void);
int gfx_band_count(void);

/* Queues the current band's send, waiting first for whichever previous
 * band's send is still in flight (gfx_band_ring_must_wait(), gfx_band.h) -
 * never for the one just queued. */
void gfx_band_submit(void);

/*
 * GFX_PIXFMT_INDEXED8 - a persistent index image gfx owns instead of an
 * RGB565 band, valid only between a gfx_mode_enter() request carrying that
 * pixfmt and the matching gfx_mode_exit(). The app writes indices; gfx
 * expands them through a LUT and sends them on the present task the next
 * time it calls gfx_present_begin()/gfx_present_wait() - the same two
 * calls it already uses for GFX_LAYOUT_FULL_FB, unchanged.
 */

/* Row-major, gfx_mode_current()->index_grid_w bytes per row. Write only the
 * cells that changed and gfx_mark_dirty() the matching panel-pixel
 * rectangle (index cell (cx, cy) is panel pixels
 * [cx*cell_size, (cx+1)*cell_size) x [cy*cell_size, (cy+1)*cell_size)) -
 * gfx never marks a write dirty on the app's behalf, the same contract
 * gfx_framebuffer() already carries. */
uint8_t* gfx_indexed_image(void);

/* Installs the 256-entry LUT GFX_PIXFMT_INDEXED8 expands through when 16-
 * colour dithering (below) is off. Copied, not referenced: the caller's
 * own table may be `static const` and go out of scope. */
void gfx_indexed_set_lut(const gfx_color_t lut[GFX_INDEXED_PALETTE_SIZE]);

/* Installs the precomputed (index, Bayer phase) -> RGB565 table 16-colour
 * mode expands through instead - see gfx_indexed.h's own comment. */
void gfx_indexed_set_lut16(const gfx_color_t dither16_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES]);

/* Selects which of the two installed LUTs GFX_PIXFMT_INDEXED8 expands
 * through - off is the 256-colour path, on is the dithered 16-colour one.
 * Both LUTs stay installed either way, so switching is free. */
void gfx_indexed_set_dither16(bool enabled);

/* Lever 2: installs `table` for `mode` and makes it the active SPATIAL
 * pattern 16-colour mode expands through (gfx_indexed_set_dither16(true))
 * - see gfx_dither_mode_t's own comment (gfx_indexed.h) for what each
 * mode's table must hold. Meaningless in 256 mode. Safe only between
 * frames, on the present task, like every other indexed setter here. */
void gfx_indexed_set_dither(gfx_dither_mode_t mode, const gfx_color_t* table);

/* True if [row0, row1) needs rendering and sending this frame - fed by the
 * ordinary gfx_mark_dirty() calls an app and ui.c already make. A true
 * return also gives the even-rounded column span (out_x0/out_x1) worth
 * touching. Always true, full width, right after gfx_mode_enter() and any
 * frame following gfx_invalidate(). */
bool gfx_band_dirty(int row0, int row1, int* out_x0, int* out_x1);

/* The band gfx_band_next() just handed out needs no redraw - advances past
 * it without rendering or sending, in place of gfx_band_submit(). */
void gfx_band_skip(void);

/* Test-only, always declared: an unsigned trip counter for the present-in-
 * flight guard above, and whether one is in flight right now. Both return
 * inert values (0 / false) wherever GFX_PRESENT_GUARD() itself folds to
 * nothing - see gfx_present_guard.h. */
unsigned gfx_present_guard_trip_count(void);
bool gfx_present_in_flight(void);

/* Test-only, always declared: an unsigned trip counter for the
 * framebuffer-availability guard (gfx_fb_guard.h) that every drawing
 * primitive checks before touching the framebuffer. */
unsigned gfx_fb_guard_trip_count(void);

/* Runtime toggle for the panel-grid overlay layer: outlines whichever grid
 * cells are actually sent each frame, cyan for a full-row send and yellow
 * for a gathered run. Off by default even in a development build: it
 * draws over real content, so it should be opted into, not always on.
 * Independent of the leaf layer below. Declared only under
 * CONFIG_LAUNCHER_DEVELOPMENT so calling it from a non-development file
 * fails to compile rather than silently no-opping. */
#if CONFIG_LAUNCHER_DEVELOPMENT
void gfx_set_debug_overlay(bool on);
bool gfx_debug_overlay(void);

/* A second, fully independent overlay layer, not a refinement of the one
 * above: outlines the leaves gfx_dirty.h's dirty_mark() actually marked
 * dirty this frame, in green - the leaves that were really touched, not
 * the static leaf lattice. Leaf bits are only ever set by a caller that
 * hands dirty_mark() a real box (see mark_leaves()) - mark_band() never
 * marks leaves, so a region only touched that way legitimately shows
 * nothing here; that is a consequence of the design, not a bug. */
void gfx_set_leaf_overlay(bool on);
bool gfx_debug_leaf_overlay(void);

/* Per-strip counts of which send path the last stretch of gfx_present()
 * calls actually took - full-band, a gathered send of runs, or a
 * full-width send at less than the whole band's height - for a device
 * test to log alongside its own timing rather than guessing the split
 * from the number alone. Reset explicitly, not by gfx_present() itself,
 * so a caller can accumulate across exactly the frames it is measuring. */
void gfx_reset_strip_send_counts(void);
void gfx_get_strip_send_counts(int* full_bands, int* gathered, int* partial_bands);

/* Panel-format bytes queued since the last gfx_reset_strip_send_counts() -
 * every send path alike, so a device test can compare pixel formats that
 * have no strip/gather distinction of their own (GFX_PIXFMT_INDEXED8)
 * against ones that do. */
int64_t gfx_get_bytes_sent(void);

/* The part of gfx_get_bytes_sent() that heal strips added. */
int64_t gfx_get_heal_bytes_sent(void);

/* Test-only: every strip of the framebuffer sent as a full band, bypassing
 * every dirty-tracking decision gfx_present() makes - the bus-time side of
 * a full present. */
void gfx_present_raw_full_frame_for_test(void);
#endif

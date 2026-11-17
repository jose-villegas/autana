/* gfx: the screen as an app sees it. The shell and applications share one
 * RGB565 framebuffer; a second full-size buffer would require a per-frame
 * PSRAM copy. Drawing belongs to draw/draw.h; presentation, the frame loop's
 * side, to gfx/present/. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "build/build_variant.h"
#include "gfx/present/modes/indexed/gfx_indexed.h"
#include "gfx/target/gfx_color.h"
#include "image/image.h"
#include "image/palette/palette.h"
#include "math/geometry/recti.h"

/* The panel's size as literals, so no caller needs the board's headers; the
 * present side asserts them against the board's own values. */
#define GFX_WIDTH             368
#define GFX_HEIGHT            448
#define GFX_FRAMEBUFFER_BYTES (GFX_WIDTH * GFX_HEIGHT * 2u)
#define GFX_SCREEN_RECT       ((recti_t){0, 0, GFX_WIDTH, GFX_HEIGHT})

_Static_assert(sizeof(gfx_color_t) == 2, "framebuffer pixels must be two bytes");

/* Direct access, for renderers that write pixels in bulk (the 3D rasterizer
 * writes here directly rather than going through draw_pixel per fragment).
 * Whatever is written must be marked with gfx_mark_dirty(). */
gfx_color_t* gfx_framebuffer(void);

/* gfx_present() sends only what changed: the panel holds the rest in its own
 * GRAM, and sending is almost the whole cost of a frame. Every draw_* call
 * marks what it touched; code writing through gfx_framebuffer() MUST mark
 * what it wrote, or the screen shows stale, not a crash. */
/* Tracked as a real box per grid cell, so a caller that touched part of a
 * cell may send less than all of it. */
void gfx_mark_dirty(int x, int y, int w, int h);
void gfx_mark_all_dirty(void);

/* One call for a transition instead of composing gfx_mark_all_dirty() and
 * a band invalidate, plus a pending latch an app's optional invalidate()
 * callback (app.h) answers to. Sets state only and frees nothing, so it is
 * safe from anywhere on core 0, an app callback or a UI build included. */
void gfx_request_full_redraw(void);

/*
 * Modes: which layout the framebuffer is held in (full, a band ring, or an
 * index image). A request asks for a layout and interlace; the grant
 * records the available layout and geometry.
 */
typedef enum {
    GFX_LAYOUT_FULL_FB,   /* one PSRAM framebuffer; core-1 present reads it */
    GFX_LAYOUT_BANDS,     /* a 2-band ring in internal SRAM; no PSRAM framebuffer */
    GFX_LAYOUT_INDEXED,   /* a persistent index image in internal RAM; core-1 expands it */
    GFX_LAYOUT_DOUBLE_FB, /* two PSRAM framebuffers; redraw every pixel every frame */
} gfx_layout_t;

typedef struct {
    gfx_layout_t layout;
    bool interlace_x; /* render-side: skip alternate columns */
    bool interlace_y; /* render-side: skip alternate rows */
    int index_grid_w; /* GFX_LAYOUT_INDEXED only: the index image's own size */
    int index_grid_h;
    int cell_size; /* GFX_LAYOUT_INDEXED only: panel pixels per index cell */
} gfx_mode_request_t;

typedef struct {
    gfx_layout_t layout;
    bool interlace_x;
    bool interlace_y;
    int width; /* granted pixel geometry, of the panel */
    int height;
    int band_height; /* the granted band height, or 0 outside GFX_LAYOUT_BANDS */
    int index_grid_w;
    int index_grid_h;
    int cell_size;
} gfx_mode_t;

static inline bool
gfx_mode_has_framebuffer(const gfx_mode_t* mode) {
    return mode->layout == GFX_LAYOUT_FULL_FB || mode->layout == GFX_LAYOUT_DOUBLE_FB;
}

/* Grants `request`, allocates whatever the granted layout needs, and
 * returns the grant; interlace in a grant only describes the request. Asserts the current mode is already GFX_LAYOUT_FULL_FB:
 * nesting one app's mode inside another's is not supported. */
const gfx_mode_t* gfx_mode_enter(const gfx_mode_request_t* request);

/* Double mode drains its send and keeps the front: the last presented frame,
 * or its clear colour if clear-on-send was on.
 * Frees whatever the current mode allocated and restores GFX_LAYOUT_FULL_FB
 * at full resolution, no interlace, the mode every app but the one just
 * exiting assumes is already in force. */
void gfx_mode_exit(void);

const gfx_mode_t* gfx_mode_current(void);

/*
 * GFX_LAYOUT_INDEXED: a persistent index image gfx owns instead of the
 * PSRAM framebuffer, valid between a matching gfx_mode_enter()/gfx_mode_exit().
 * The app writes indices; the present task expands them through a LUT on the
 * same present path as GFX_LAYOUT_FULL_FB.
 */

/* An IMAGE_INDEXED8 image, index_grid_w by index_grid_h. Write only the
 * cells that changed and gfx_mark_dirty() the matching panel-pixel
 * rectangle (index cell (cx, cy) is panel pixels
 * [cx*cell_size, (cx+1)*cell_size) x [cy*cell_size, (cy+1)*cell_size));
 * gfx never marks a write dirty on the app's behalf, the same contract
 * gfx_framebuffer() already carries. */
image_t gfx_indexed_image(void);

/* Installs the GFX_INDEXED_PALETTE_SIZE-entry palette GFX_LAYOUT_INDEXED
 * expands through when 16-colour dithering (below) is off. Copied, not
 * referenced: the caller's own table may be `static const` and go out of
 * scope. */
void gfx_indexed_set_palette(const palette_t* palette);

/* Installs the precomputed (index, Bayer phase) -> RGB565 table 16-colour
 * mode expands through instead; see gfx_indexed.h's own comment. */
void gfx_indexed_set_lut16(const gfx_color_t dither16_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES]);

/* Selects which of the two installed LUTs GFX_LAYOUT_INDEXED expands
 * through: off is the 256-colour path, on is the dithered 16-colour one.
 * Both LUTs stay installed either way, so switching is free. */
void gfx_indexed_set_dither16(bool enabled);

/* Installs `table` for `mode` and makes it the active SPATIAL
 * pattern 16-colour mode expands through (gfx_indexed_set_dither16(true));
 * see gfx_dither_mode_t's own comment (gfx_indexed.h) for what each
 * mode's table must hold. Meaningless in 256 mode. Safe only between
 * frames, on the present task, like every other indexed setter here. */
void gfx_indexed_set_dither(gfx_dither_mode_t mode, const gfx_color_t* table);

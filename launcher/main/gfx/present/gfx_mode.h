/*
 * gfx_mode: the mode-grant arithmetic behind gfx_mode_enter(), as a
 * standalone, ESP-IDF-free module so a host suite can drive it without a
 * framebuffer or a panel.
 *
 * An app declares what it wants at enter(): a layout, a resolution, and a
 * per-axis interlace choice. gfx_mode_resolve() is the pure function
 * that turns a request into a grant; gfx_mode_enter() (gfx_mode.c) is the only
 * caller that also allocates. All layouts are wired to real rendering,
 * but only GFX_RESOLUTION_FULL with no interlace is (no caller ever
 * requests HALF or turns interlace on), so those fields exist ahead of a
 * caller that needs them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/gfx.h"
#include "gfx/present/gfx_indexed.h"

typedef enum {
    GFX_LAYOUT_FULL_FB, /* one PSRAM framebuffer; core-1 present reads it */
    GFX_LAYOUT_BANDS,   /* a 2-band ring in internal SRAM; no PSRAM framebuffer */
    GFX_LAYOUT_INDEXED, /* a persistent index image in internal RAM; core-1 expands it */
} gfx_layout_t;

/* Ordered least-restrictive first, so resolving a grant is "whichever of
 * request and system max asks for less"; see gfx_mode_resolve() below,
 * where the higher ordinal (the smaller resolution) always wins. */
typedef enum {
    GFX_RESOLUTION_FULL = 0,
    GFX_RESOLUTION_HALF = 1,
} gfx_resolution_t;

typedef struct {
    gfx_layout_t layout;
    gfx_resolution_t resolution;
    bool interlace_x; /* render-side: skip alternate columns */
    bool interlace_y; /* render-side: skip alternate rows */
    int index_grid_w; /* GFX_LAYOUT_INDEXED only: the index image's own size */
    int index_grid_h;
    int cell_size; /* GFX_LAYOUT_INDEXED only: panel pixels per index cell */
} gfx_mode_request_t;

typedef struct {
    gfx_layout_t layout;
    gfx_resolution_t resolution;
    bool interlace_x;
    bool interlace_y;
    int width; /* granted pixel geometry, after resolution */
    int height;
    int band_height; /* the granted band height, or 0 outside GFX_LAYOUT_BANDS */
    int index_grid_w;
    int index_grid_h;
    int cell_size;
} gfx_mode_t;

/* Resolves a request against the system's resolution cap and the panel's own
 * geometry, without touching any buffer. `full_width`/`full_height` are the
 * panel's real geometry (GFX_WIDTH/GFX_HEIGHT on the device) and
 * `full_band_height` is GFX_BAND_HEIGHT, passed in rather than read from a
 * macro so a host suite can drive this with its own numbers. Layout and
 * interlace pass through unchanged: gfx does not yet cap either. */
static inline gfx_mode_t
gfx_mode_resolve(const gfx_mode_request_t* request, gfx_resolution_t system_max, int full_width, int full_height,
                 int full_band_height) {
    gfx_mode_t granted;

    granted.layout = request->layout;
    granted.resolution = (request->resolution > system_max) ? request->resolution : system_max;
    granted.interlace_x = request->interlace_x;
    granted.interlace_y = request->interlace_y;
    granted.index_grid_w = request->index_grid_w;
    granted.index_grid_h = request->index_grid_h;
    granted.cell_size = request->cell_size;

    const int divisor = (granted.resolution == GFX_RESOLUTION_HALF) ? 2 : 1;
    granted.width = full_width / divisor;
    granted.height = full_height / divisor;
    granted.band_height = (granted.layout == GFX_LAYOUT_BANDS) ? full_band_height / divisor : 0;

    return granted;
}

/*
 * Mode: a full PSRAM framebuffer, an internal-SRAM band ring for a
 * transient renderer, or a persistent internal-RAM index image.
 * Requested from enter(), released with gfx_mode_exit() from exit(). No
 * caller ever asks for anything but full resolution; an interlace request
 * is granted (gfx_mode.h) but changes nothing drawn; gfx_set_interlace()
 * is the switch that does.
 */

/* Grants `request`, allocates whatever the granted layout needs, and
 * returns the grant. Asserts the current mode is already GFX_LAYOUT_FULL_FB:
 * nesting one app's mode inside another's is not supported. */
const gfx_mode_t* gfx_mode_enter(const gfx_mode_request_t* request);

/* Frees whatever the current mode allocated and restores GFX_LAYOUT_FULL_FB
 * at full resolution, no interlace, the mode every app but the one just
 * exiting assumes is already in force. */
void gfx_mode_exit(void);

const gfx_mode_t* gfx_mode_current(void);

/*
 * GFX_LAYOUT_INDEXED: a persistent index image gfx owns instead of the
 * PSRAM framebuffer, valid between a matching gfx_mode_enter()/gfx_mode_exit().
 * The app writes indices; the present task expands them through a LUT on the
 * same gfx_present_begin()/gfx_present_wait() path as GFX_LAYOUT_FULL_FB.
 */

/* Row-major, gfx_mode_current()->index_grid_w bytes per row. Write only the
 * cells that changed and gfx_mark_dirty() the matching panel-pixel
 * rectangle (index cell (cx, cy) is panel pixels
 * [cx*cell_size, (cx+1)*cell_size) x [cy*cell_size, (cy+1)*cell_size));
 * gfx never marks a write dirty on the app's behalf, the same contract
 * gfx_framebuffer() already carries. */
uint8_t* gfx_indexed_image(void);

/*
 * Readback: the frame on the panel, row by row, for a capture. A
 * framebuffer or index image is readable at once. In GFX_LAYOUT_BANDS the
 * first gfx_readback_begin() is PENDING until a frame has redrawn every
 * band into a PSRAM copy; every band sent after that updates the copy
 * until the mode exits, so later calls are READY at once, frozen loop
 * included. Pair each with gfx_readback_end().
 */
typedef enum {
    GFX_READBACK_READY,
    GFX_READBACK_PENDING,
    GFX_READBACK_UNAVAILABLE, /* band mode, and no room for its snapshot */
} gfx_readback_t;

gfx_readback_t gfx_readback_begin(void);

/* Panel row `y`, exactly as sent: the index image expanded as the present
 * path expands it. Only after gfx_readback_begin() answered READY. */
void gfx_read_panel_row(int y, gfx_color_t out_row[GFX_WIDTH]);

/* Ends a capture. Band mode's copy stays until gfx_mode_exit(). */
void gfx_readback_end(void);

/* Installs the 256-entry LUT GFX_LAYOUT_INDEXED expands through when 16-
 * colour dithering (below) is off. Copied, not referenced: the caller's
 * own table may be `static const` and go out of scope. */
void gfx_indexed_set_lut(const gfx_color_t lut[GFX_INDEXED_PALETTE_SIZE]);

/* Installs the precomputed (index, Bayer phase) -> RGB565 table 16-colour
 * mode expands through instead; see gfx_indexed.h's own comment. */
void gfx_indexed_set_lut16(const gfx_color_t dither16_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES]);

/* Selects which of the two installed LUTs GFX_LAYOUT_INDEXED expands
 * through: off is the 256-colour path, on is the dithered 16-colour one.
 * Both LUTs stay installed either way, so switching is free. */
void gfx_indexed_set_dither16(bool enabled);

/* Lever 2: installs `table` for `mode` and makes it the active SPATIAL
 * pattern 16-colour mode expands through (gfx_indexed_set_dither16(true));
 * see gfx_dither_mode_t's own comment (gfx_indexed.h) for what each
 * mode's table must hold. Meaningless in 256 mode. Safe only between
 * frames, on the present task, like every other indexed setter here. */
void gfx_indexed_set_dither(gfx_dither_mode_t mode, const gfx_color_t* table);

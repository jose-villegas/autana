/*
 * gfx_mode - the mode-grant arithmetic behind gfx_mode_enter(), as a
 * standalone, ESP-IDF-free module so a host suite can drive it without a
 * framebuffer or a panel.
 *
 * An app declares what it wants at enter(): a layout (one PSRAM framebuffer,
 * or a band ring in internal SRAM for a full-redraw renderer), a resolution,
 * and a per-axis interlace choice. gfx_mode_resolve() is the pure function
 * that turns a request into a grant; gfx_mode_enter() (gfx.c) is the only
 * caller that also allocates. Both layouts are wired to real rendering,
 * but only GFX_RESOLUTION_FULL with no interlace is - no caller ever
 * requests HALF or turns interlace on - so those fields exist ahead of a
 * caller that needs them.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    GFX_LAYOUT_FULL_FB, /* one PSRAM framebuffer; core-1 present reads it */
    GFX_LAYOUT_BANDS,   /* a 2-band ring in internal SRAM; no PSRAM framebuffer */
} gfx_layout_t;

/* What a band holds. RGB565 is a plain pixel band, drawn by the app the way
 * GFX_LAYOUT_BANDS always has. INDEXED8 instead holds a persistent
 * grid_w x grid_h byte image of palette indices - the app writes indices,
 * never pixels, and the present task expands them through a 256-entry LUT
 * while upscaling by `cell_size` into the band. Meaningless outside
 * GFX_LAYOUT_BANDS. */
typedef enum {
    GFX_PIXFMT_RGB565,
    GFX_PIXFMT_INDEXED8,
} gfx_pixfmt_t;

/* Ordered least-restrictive first, so resolving a grant is "whichever of
 * request and system max asks for less" - see gfx_mode_resolve() below,
 * where the higher ordinal (the smaller resolution) always wins. */
typedef enum {
    GFX_RESOLUTION_FULL = 0,
    GFX_RESOLUTION_HALF = 1,
} gfx_resolution_t;

typedef struct {
    gfx_layout_t layout;
    gfx_resolution_t resolution;
    bool interlace_x;    /* render-side: skip alternate columns */
    bool interlace_y;    /* render-side: skip alternate rows */
    gfx_pixfmt_t pixfmt; /* GFX_LAYOUT_BANDS only; ignored otherwise */
    int index_grid_w;    /* GFX_PIXFMT_INDEXED8 only: the index image's own size */
    int index_grid_h;
    int cell_size; /* GFX_PIXFMT_INDEXED8 only: panel pixels per index cell */
} gfx_mode_request_t;

typedef struct {
    gfx_layout_t layout;
    gfx_resolution_t resolution;
    bool interlace_x;
    bool interlace_y;
    int width; /* granted pixel geometry, after resolution */
    int height;
    int band_height; /* the granted band height, or 0 for GFX_LAYOUT_FULL_FB */
    gfx_pixfmt_t pixfmt;
    int index_grid_w;
    int index_grid_h;
    int cell_size;
} gfx_mode_t;

/* Resolves a request against the system's resolution cap and the panel's own
 * geometry, without touching any buffer. `full_width`/`full_height` are the
 * panel's real geometry (GFX_WIDTH/GFX_HEIGHT on the device) and
 * `full_band_height` is GFX_BAND_HEIGHT - passed in rather than read from a
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
    granted.pixfmt = request->pixfmt;
    granted.index_grid_w = request->index_grid_w;
    granted.index_grid_h = request->index_grid_h;
    granted.cell_size = request->cell_size;

    const int divisor = (granted.resolution == GFX_RESOLUTION_HALF) ? 2 : 1;
    granted.width = full_width / divisor;
    granted.height = full_height / divisor;
    granted.band_height = (granted.layout == GFX_LAYOUT_BANDS) ? full_band_height / divisor : 0;

    return granted;
}

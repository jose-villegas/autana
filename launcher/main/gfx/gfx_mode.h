/*
 * gfx_mode: the mode-grant arithmetic behind gfx_mode_enter(), as a
 * standalone, ESP-IDF-free module so a host suite can drive it without a
 * framebuffer or a panel. A request asks for a layout and interlace; the
 * grant records the available layout and geometry.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    GFX_LAYOUT_FULL_FB, /* one PSRAM framebuffer; core-1 present reads it */
    GFX_LAYOUT_BANDS,   /* a 2-band ring in internal SRAM; no PSRAM framebuffer */
    GFX_LAYOUT_INDEXED, /* a persistent index image in internal RAM; core-1 expands it */
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

/* Panel and band geometry are passed in so host suites can grant modes
 * without device macros. Layout and interlace requests pass through. */
static inline gfx_mode_t
gfx_mode_resolve(const gfx_mode_request_t* request, int full_width, int full_height, int full_band_height) {
    gfx_mode_t granted;

    granted.layout = request->layout;
    granted.interlace_x = request->interlace_x;
    granted.interlace_y = request->interlace_y;
    granted.index_grid_w = request->index_grid_w;
    granted.index_grid_h = request->index_grid_h;
    granted.cell_size = request->cell_size;

    granted.width = full_width;
    granted.height = full_height;
    granted.band_height = (granted.layout == GFX_LAYOUT_BANDS) ? full_band_height : 0;

    return granted;
}

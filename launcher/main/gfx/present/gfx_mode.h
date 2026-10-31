/*
 * gfx_mode: the frame loop's side of the modes gfx.h declares - the half
 * picture, its expansion and readback of the panel's frame.
 * gfx_mode_resolve() is the pure grant arithmetic a host suite drives
 * without a framebuffer or a panel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/gfx.h"
#include "gfx/present/modes/band/gfx_band_run.h"

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

/* Exact-half PSRAM picture, allocated lazily and freed on mode exit.
 * Returns NULL outside GFX_LAYOUT_FULL_FB or when allocation fails. */
gfx_color_t* gfx_half_picture(void);

/* Presents the half picture doubled in both axes, with every strip dirty.
 * Raw framebuffer drawing is refused until that present completes. */
void gfx_expand_frame(void);
/* gfx_expand_frame() blended bilinearly except across the seams (gfx_target.h)
 * marked in gfx_half_seams(). */
void gfx_expand_frame_blended(void);
bool gfx_frame_expanded(void);

/* The half picture's seam mask, GFX_SEAM_BYTES of the half size; NULL with no half picture. */
uint8_t* gfx_half_seams(void);

/* Replayed over each expanded strip, after its picture is doubled. */
void gfx_set_frame_overlay(gfx_band_overlay_fn overlay);
gfx_band_overlay_fn gfx_frame_overlay(void);

/*
 * Readback: the frame on the panel, row by row, for a capture. A
 * double framebuffer drains its send first. Other framebuffers and index
 * images are readable at once. In GFX_LAYOUT_BANDS the
 * first gfx_readback_begin() is PENDING until a frame has redrawn every
 * band into a PSRAM copy; every band sent after that updates the copy
 * until the mode exits, so later calls are READY at once, frozen loop
 * included. Pair each with gfx_readback_end().
 */
typedef enum {
    GFX_READBACK_READY,
    GFX_READBACK_PENDING,
    GFX_READBACK_UNAVAILABLE, /* band mode with no room for its snapshot, or a shown frame cleared on send */
} gfx_readback_t;

gfx_readback_t gfx_readback_begin(void);

/* Panel row `y`, exactly as sent: the index image expanded as the present
 * path expands it. Only after gfx_readback_begin() answered READY. */
void gfx_read_panel_row(int y, gfx_color_t out_row[GFX_WIDTH]);

/* Ends a capture. Band mode's copy stays until gfx_mode_exit(). */
void gfx_readback_end(void);

/*
 * gfx_band - the two-slot band-ring state machine behind gfx_band_next()/
 * gfx_band_submit(), as a standalone, ESP-IDF-free module so a host suite
 * can drive it without a panel, DMA, or a semaphore. Also the span geometry
 * gfx_band_submit() sends less than a full band through: even-rounding and
 * clipping a column range, and packing a band's rows down to it in place.
 *
 * Two buffers only: a full-redraw renderer draws band k+1 into the slot NOT
 * currently sending while band k's DMA transfer is still in flight, and the
 * only wait is for band k's own transfer to land before band k+1's own send
 * can be queued - queuing any earlier would put fresh content on a buffer
 * still on the wire. gfx.c's gfx_band_submit() is where that wait actually
 * blocks; here it is just a boolean the ring computes from how many bands
 * have been handed out and which one, if any, is still in flight.
 */
#pragma once

#include <stdbool.h>
#include <string.h>

#include "gfx/gfx_color.h"
#include "util/intmath.h"

#define GFX_BAND_SLOTS 2

typedef struct {
    int band_count;  /* total bands this frame */
    int next_render; /* index of the band the next gfx_band_ring_slot() hands out */
    int in_flight;   /* index of the band whose send is queued but not waited on, or -1 */
} gfx_band_ring_t;

static inline void
gfx_band_ring_begin(gfx_band_ring_t* ring, int band_count) {
    ring->band_count = band_count;
    ring->next_render = 0;
    ring->in_flight = -1;
}

/* True once every band this frame has been handed out. */
static inline bool
gfx_band_ring_done(const gfx_band_ring_t* ring) {
    return ring->next_render >= ring->band_count;
}

/* Which of the two buffers the next render targets. */
static inline int
gfx_band_ring_slot(const gfx_band_ring_t* ring) {
    return ring->next_render % GFX_BAND_SLOTS;
}

/* The absolute row the next band starts at, given the band height in force. */
static inline int
gfx_band_ring_row0(const gfx_band_ring_t* ring, int band_height) {
    return ring->next_render * band_height;
}

/* True if submitting the next band's send must first wait for a previous
 * one - false only for band 0, which has nothing in flight yet. */
static inline bool
gfx_band_ring_must_wait(const gfx_band_ring_t* ring) {
    return ring->in_flight >= 0;
}

/* Called once a band's send has been queued (after waiting on the previous
 * one, if gfx_band_ring_must_wait() said so) - advances the ring so the
 * next gfx_band_ring_slot()/gfx_band_ring_row0() describe the following
 * band. */
static inline void
gfx_band_ring_advance(gfx_band_ring_t* ring) {
    ring->in_flight = ring->next_render;
    ring->next_render++;
}

/* Advances past the current band WITHOUT sending it - the caller decided
 * this band needs no redraw this frame. Nothing new is in flight, so
 * whatever the ring was already waiting on (if anything) is unaffected;
 * only gfx_band_ring_advance() ever changes in_flight. */
static inline void
gfx_band_ring_skip(gfx_band_ring_t* ring) {
    ring->next_render++;
}

/* True once the last band handed out has also been waited for - what a
 * frame's closing wait checks before it can skip its own wait. */
static inline bool
gfx_band_ring_settled(const gfx_band_ring_t* ring) {
    return ring->in_flight < 0;
}

static inline void
gfx_band_ring_settle(gfx_band_ring_t* ring) {
    ring->in_flight = -1;
}

/* Rounds [x0, x1) outward to even panel columns (util/intmath.h) and clips
 * to [0, width) - width is always even (GFX_WIDTH), so clipping first and
 * rounding after can never push the result back out of range, which is why
 * gfx_band_submit() needs no further clamp once this returns. Returns
 * false, leaving the outputs undefined, when nothing survives - an empty or
 * fully-off-band request. */
static inline bool
gfx_band_span_clip(int x0, int x1, int width, int* out_x0, int* out_x1) {
    x0 = x0 < 0 ? 0 : (x0 > width ? width : x0);
    x1 = x1 < 0 ? 0 : (x1 > width ? width : x1);
    x0 = even_floor(x0);
    x1 = even_ceil(x1);
    if (x0 >= x1) {
        return false;
    }
    *out_x0 = x0;
    *out_x1 = x1;
    return true;
}

/* Packs `height` rows of `buf` (stride `width`) down to columns [x0, x1),
 * contiguous, in place, so gfx_band_submit() can hand the panel one flat
 * buffer - esp_lcd_panel_draw_bitmap() takes no stride, and a call per row measured 5.4x
 * slower (docs/notes/Display-and-Rendering.md, "Still untapped"). A no-op
 * at full width. memmove, not memcpy: a wide span overlaps its own source
 * row. Rows go low first, and row r's packed end never reaches row r+1's
 * source. */
static inline void
gfx_band_span_pack(gfx_color_t* buf, int width, int height, int x0, int x1) {
    const int span_w = x1 - x0;
    if (x0 == 0 && span_w == width) {
        return;
    }
    for (int row = 0; row < height; row++) {
        memmove(buf + (size_t)row * span_w, buf + (size_t)row * width + x0, (size_t)span_w * sizeof(gfx_color_t));
    }
}

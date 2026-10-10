/*
 * icon: draws an icon, a one-bit image (GFX_IMAGE_MONO1, gfx/image/gfx_image.h)
 * read in place from a pack, as runs of filled rectangles. Icons are found by
 * name once, when their owner loads them; drawing streams each run to a
 * callback rather than collecting them, so a draw's stack is O(1) in the
 * icon's size.
 *
 * No allocation and no ESP-IDF.
 */
#pragma once

#include "gfx/image/gfx_image.h"

/* One run, in whatever coordinate space its producer documents: a carrier
 * for a caller that wants icon_walk_blocks()'s runs collected into an array
 * instead of streamed. */
typedef struct {
    int x, y, w, h;
} icon_rect_t;

/* icon_walk_blocks()'s output: one horizontal run, already scaled and
 * placed relative to the destination box's own origin (0, 0); the caller
 * adds its box's x/y. */
typedef void (*icon_emit_fn)(void* ctx, int x, int y, int w, int h);

/* Fits `icon`'s ink to a `box_w` x `box_h` box at the largest whole scale,
 * centred, and emits each run. `icon` is GFX_IMAGE_MONO1. Icons draw once
 * per repaint of their canvas, not in a hot loop, so the callback costs
 * nothing worth measuring. */
void icon_walk_blocks(const gfx_image_t* icon, int box_w, int box_h, icon_emit_fn emit, void* ctx);

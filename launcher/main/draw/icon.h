/* Icons stream one-bit images from pack entries as rectangle runs.
 * Streaming keeps draw stack usage independent of icon size, with no
 * allocation or device dependency. */
#pragma once

#include "math/geometry/recti.h"

#include "image/image.h"

/* icon_walk_blocks()'s output: one horizontal run, already scaled and
 * placed relative to the destination box's own origin (0, 0); the caller
 * adds its box's x/y. */
typedef void (*icon_emit_fn)(void* ctx, recti_t rect);

/* Fits `icon`'s ink to a `box_w` x `box_h` box at the largest whole scale,
 * centred, and emits each run. `icon` is IMAGE_MONO1. Icons draw once
 * per repaint of their canvas, not in a hot loop, so the callback costs
 * nothing worth measuring. */
void icon_walk_blocks(const image_t* icon, int box_w, int box_h, icon_emit_fn emit, void* ctx);

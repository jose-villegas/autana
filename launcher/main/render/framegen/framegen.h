/*
 * framegen: a generated frame, the last drawn picture forward-warped from the
 * view it was drawn through to a later one, by its depth (docs/render/frame/Frame-Generation.md).
 * Removable as this folder plus the render context's frame generation setting.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/raster.h"
#include "render/render_view.h"

/* Frames shown per drawn frame with generation on: the drawn one and the one
 * generated from it. */
#define FRAMEGEN_SHOWN_PER_DRAWN 2

/* A warped sky pixel in the target: farther than anything drawn, yet not a
 * hole; the fill shows it as the sky (R3D_DEPTH_EMPTY). */
#define FRAMEGEN_DEPTH_SKY       1U

/* A picture at the raster's size, in PSRAM: the warp's target, its colour
 * and the depth its splats test, R3D_DEPTH_EMPTY where nothing landed; or the
 * still part of a drawn frame, before its moving instances drew
 * (raster_t.still_colour). */
typedef struct {
    uint16_t* colour;
    uint16_t* depth;
    int width, height;
} framegen_target_t;

/* Allocates `out` for a raster of `width` x `height`; false, with `out` empty,
 * when there is no memory. */
bool framegen_target_alloc(framegen_target_t* out, int width, int height);
void framegen_target_free(framegen_target_t* target);

/* Warps `source`, or the raster's own picture when NULL, drawn through
 * `drawn` at the raster's depth scale, to `now`: each pixel lands on the
 * nearest target pixel, the nearer point winning; the sky only turns. Both
 * cores take half the source rows. */
void framegen_warp(const raster_t* raster, const framegen_target_t* source, const render_view_t* drawn,
                   const render_view_t* now, framegen_target_t* out);

/* Completes the generated frame in the raster's own picture, so it composes
 * as a drawn frame does and moving instances can draw over it: `target`'s
 * pixel where one landed, `source`'s (the raster's own when NULL) where
 * nothing did. Returns the pixels nothing landed on. */
int framegen_fill(const raster_t* raster, const framegen_target_t* source, const framegen_target_t* target);

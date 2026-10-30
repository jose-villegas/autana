/*
 * raster_lines: development views' lines over the composed picture, given in
 * a lens's space and cut at its near plane. With a depth test, a stretch of
 * line behind what the picture drew shows dimmed: visible, but secondary.
 */
#pragma once

#include "gfx/gfx.h"
#include "image/pixel/rgb565.h"
#include "math/geometry/box3f.h"
#include "render/r3d_pipeline.h"
#include "render/raster.h"

/* What a dimmed stretch keeps of its colour by default, out of RGB565_SCALE_UNIT. */
#define RASTER_LINES_BEHIND_KEEP (RGB565_SCALE_UNIT * 3 / 8)
/* The stretch, in picture pixels, that one depth sample decides. */
#define RASTER_LINES_STEP_PX     4

/* The drawn picture's depth, read where a line crosses the composed picture. */
typedef struct {
    image_t drawn;        /* the raster's depth attachment, at its render size */
    float depth_near;     /* the encoding raster_draw() last used */
    int width, height;    /* the composed picture the lines go over */
    unsigned behind_keep; /* what a stretch behind keeps, out of RGB565_SCALE_UNIT; 0 leaves it out */
} raster_lines_depth_t;

/* The depth `raster` last drew, for lines over a `width` by `height` picture,
 * a stretch behind it keeping `behind_keep` of its colour. */
void raster_lines_depth(raster_lines_depth_t* depth, const raster_t* raster, int width, int height,
                        unsigned behind_keep);

/* From `a` to `b` in `lens`'s space; `depth` NULL draws it all at full colour. */
void raster_lines_segment(const r3d_lens_t* lens, const raster_lines_depth_t* depth, vec3f_t a, vec3f_t b,
                          gfx_color_t color);

/* The twelve edges of `box`, in the units `lens` takes. */
void raster_lines_box(const r3d_lens_t* lens, const raster_lines_depth_t* depth, box3f_t box, gfx_color_t color);

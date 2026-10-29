/*
 * upscale: nearest-neighbour row and column maps from a render size to a
 * destination size, and the copy through them; optionally blended within a
 * surface, with the seams between surfaces marked for the present to keep.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/target/gfx_target.h"
#include "image/pixel/rgb565.h"

/* How rows are filled, chosen once per size. All but UPSCALE_BLOCKS write
 * what the maps say, pixel for pixel; it repeats each pixel `factor` times. */
typedef enum {
    UPSCALE_MAPPED,  /* each destination pixel through both maps */
    UPSCALE_KEPT,    /* the width kept and even: two source pixels per 32-bit store */
    UPSCALE_DOUBLED, /* the width doubled: each source pixel one 32-bit store */
    UPSCALE_BLOCKS,  /* both axes by one integer factor, where neither above applies */
} upscale_path_t;

/* How a kept width fills its rows. Other paths are always nearest. */
typedef enum {
    UPSCALE_FILTER_NEAREST, /* each row is its nearest source row */
    UPSCALE_FILTER_SURFACE, /* blends the two source rows around it unless depth steps between them */
} upscale_filter_t;

/* Two inverse depths are one surface when they differ by at most the nearer's
 * 1 / 2^UPSCALE_SURFACE_STEP_SHIFT: a relative step in depth. */
#define UPSCALE_SURFACE_STEP_SHIFT 4

static inline bool
upscale_same_surface(uint16_t depth_a, uint16_t depth_b) {
    const uint16_t nearer = depth_a > depth_b ? depth_a : depth_b;
    const uint16_t step = depth_a > depth_b ? depth_a - depth_b : depth_b - depth_a;
    return step <= (nearer >> UPSCALE_SURFACE_STEP_SHIFT);
}

/* A destination row's blend: its lower source row, times RGB565_SCALE_UNIT,
 * plus the next source row's share of it. */
#define UPSCALE_BLEND(row, share) ((uint16_t)(((row) * RGB565_SCALE_UNIT) + (share)))

/* The share of its brightness a repeated scanline row keeps. */
#define UPSCALE_SCANLINE_KEEP     (RGB565_SCALE_UNIT / 2)

typedef struct {
    int scanline_run;
    int source_width, source_height;
    int destination_width, destination_height;
    int factor; /* UPSCALE_BLOCKS's */
    upscale_path_t path;
    upscale_filter_t filter; /* left as it is by upscale_init() */
    uint16_t* columns;
    uint16_t* rows;
    uint16_t* blends; /* per destination row, UPSCALE_BLEND; built by upscale_init_blends() */
} upscale_t;

/* Builds maps for one source and destination size. The caller retains map storage. */
bool upscale_init(upscale_t* scale, int source_width, int source_height, int destination_width, int destination_height,
                  uint16_t* columns, uint16_t* rows);

/* Scales destination rows [first_row, first_row + row_count) from `source`.
 * A zero attached depth writes `clear`. `destination` is 4-byte aligned.
 */
void upscale_rows(const upscale_t* scale, const gfx_target_t* source, uint16_t clear, uint16_t* destination,
                  int first_row, int row_count);

/* Builds `blends` into storage of destination_height entries the caller retains. */
void upscale_init_blends(upscale_t* scale, uint16_t* blends);

/* upscale_rows() through scale->filter: with UPSCALE_FILTER_SURFACE, a kept
 * width blends rows within a surface and, when `seams` is not NULL, marks
 * the seams (gfx_target.h) of rows [first_row, first_row + row_count) where
 * a pixel is another surface than the one right of it or below it.
 * Otherwise every pixel is marked. */
void upscale_rows_filtered(const upscale_t* scale, const gfx_target_t* source, uint16_t clear, uint16_t* destination,
                           uint8_t* seams, int first_row, int row_count);

/* Paired copies with uniform even runs only; other runs copy unchanged. */
void upscale_rows_scanlined(const upscale_t* scale, const gfx_target_t* source, uint16_t clear, uint16_t* destination,
                            int first_row, int row_count);

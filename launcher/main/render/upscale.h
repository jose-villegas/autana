/*
 * upscale: nearest-neighbour row and column maps from a render size to a
 * destination size, and the copy through them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx/target/gfx_render_target.h"

/* How rows are filled, chosen once per size. All but UPSCALE_BLOCKS write
 * what the maps say, pixel for pixel; it repeats each pixel `factor` times. */
typedef enum {
    UPSCALE_MAPPED,  /* each destination pixel through both maps */
    UPSCALE_KEPT,    /* the width kept and even: two source pixels per 32-bit store */
    UPSCALE_DOUBLED, /* the width doubled: each source pixel one 32-bit store */
    UPSCALE_BLOCKS,  /* both axes by one integer factor, where neither above applies */
} upscale_path_t;

/* Repeated rows retain half brightness. */
#define UPSCALE_SCANLINE_KEEP (GFX_COLOR_SCALE_UNIT / 2)

typedef struct {
    int scanline_run;
    int source_width, source_height;
    int destination_width, destination_height;
    int factor; /* UPSCALE_BLOCKS's */
    upscale_path_t path;
    uint16_t* columns;
    uint16_t* rows;
} upscale_t;

/* Builds maps for one source and destination size. The caller retains map storage. */
bool upscale_init(upscale_t* scale, int source_width, int source_height, int destination_width, int destination_height,
                  uint16_t* columns, uint16_t* rows);

/* Scales destination rows [first_row, first_row + row_count) from `source`.
 * A zero attached depth writes `clear`. `destination` is 4-byte aligned.
 */
void upscale_rows(const upscale_t* scale, const gfx_render_target_t* source, uint16_t clear, uint16_t* destination,
                  int first_row, int row_count);

/* Paired copies with uniform even runs only; other runs copy unchanged. */
void upscale_rows_scanlined(const upscale_t* scale, const gfx_render_target_t* source, uint16_t clear,
                            uint16_t* destination, int first_row, int row_count);

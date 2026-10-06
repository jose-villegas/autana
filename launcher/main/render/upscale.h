/*
 * upscale: nearest-neighbour row and column maps from a render size to a
 * destination size, and the copy through them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* How rows are filled, chosen once per size. All but UPSCALE_BLOCKS write
 * what the maps say, pixel for pixel; it repeats each pixel `factor` times. */
typedef enum {
    UPSCALE_MAPPED, /* each destination pixel through both maps */
    UPSCALE_ROWS,   /* the width kept: a mapped row copied whole */
    UPSCALE_PAIRS,  /* the width doubled: each source pixel one 32-bit store */
    UPSCALE_BLOCKS, /* both axes by one integer factor, where neither above applies */
} upscale_path_t;

typedef struct {
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

/* Scales destination rows [first_row, first_row + row_count). When `depth`
 * is present, a zero source depth writes `clear` instead of its colour.
 * `destination` is 4-byte aligned. */
void upscale_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                  uint16_t* destination, int first_row, int row_count);

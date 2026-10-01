#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int source_width, source_height;
    int destination_width, destination_height;
    int horizontal_factor, vertical_factor;
    bool integer;
    uint16_t* columns;
    uint16_t* rows;
} upscale_t;

/* Builds maps for one source and destination size. The caller retains map storage. */
bool upscale_init(upscale_t* scale, int source_width, int source_height, int destination_width, int destination_height,
                  uint16_t* columns, uint16_t* rows);

/* Scales destination rows [first_row, first_row + row_count). When `depth`
 * is present, a zero source depth writes `clear` instead of its colour. */
void upscale_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                  uint16_t* destination, int first_row, int row_count);

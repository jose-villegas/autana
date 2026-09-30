#include "render/upscale.h"

#include <assert.h>
#include <stddef.h>

static uint16_t
nearest_source(int destination, int destination_size, int source_size) {
    if (destination_size == 1) {
        return 0;
    }
    const int numerator = destination * (source_size - 1);
    return (uint16_t)((numerator + (destination_size - 1) / 2) / (destination_size - 1));
}

bool
upscale_init(upscale_t* scale, int source_width, int source_height, int destination_width, int destination_height,
             uint16_t* columns, uint16_t* rows) {
    if (scale == NULL || columns == NULL || rows == NULL || source_width < 1 || source_height < 1
        || destination_width < source_width || destination_height < source_height) {
        return false;
    }
    scale->source_width = source_width;
    scale->source_height = source_height;
    scale->destination_width = destination_width;
    scale->destination_height = destination_height;
    scale->horizontal_factor = destination_width / source_width;
    scale->vertical_factor = destination_height / source_height;
    scale->integer = destination_width % source_width == 0 && destination_height % source_height == 0
                     && scale->horizontal_factor == scale->vertical_factor;
    scale->columns = columns;
    scale->rows = rows;
    for (int x = 0; x < destination_width; x++) {
        columns[x] = nearest_source(x, destination_width, source_width);
    }
    for (int y = 0; y < destination_height; y++) {
        rows[y] = nearest_source(y, destination_height, source_height);
    }
    return true;
}

static void
upscale_integer_rows(const upscale_t* scale, const uint16_t* source, uint16_t* destination, int first_row,
                     int row_count) {
    for (int y = first_row; y < first_row + row_count; y++) {
        const uint16_t* input = source + (size_t)(y / scale->vertical_factor) * scale->source_width;
        uint16_t* output = destination + (size_t)y * scale->destination_width;
        for (int x = 0; x < scale->source_width; x++) {
            const uint16_t pixel = input[x];
            for (int repeat = 0; repeat < scale->horizontal_factor; repeat++) {
                output[x * scale->horizontal_factor + repeat] = pixel;
            }
        }
    }
}

static void
upscale_mapped_rows(const upscale_t* scale, const uint16_t* source, uint16_t* destination, int first_row,
                    int row_count) {
    for (int y = first_row; y < first_row + row_count; y++) {
        const uint16_t* input = source + (size_t)scale->rows[y] * scale->source_width;
        uint16_t* output = destination + (size_t)y * scale->destination_width;
        for (int x = 0; x < scale->destination_width; x++) {
            output[x] = input[scale->columns[x]];
        }
    }
}

void
upscale_rows(const upscale_t* scale, const uint16_t* source, uint16_t* destination, int first_row, int row_count) {
    assert(scale != NULL);
    assert(source != NULL);
    assert(destination != NULL);
    assert(first_row >= 0);
    assert(row_count >= 0);
    assert(first_row + row_count <= scale->destination_height);
    if (scale->integer) {
        upscale_integer_rows(scale, source, destination, first_row, row_count);
    } else {
        upscale_mapped_rows(scale, source, destination, first_row, row_count);
    }
}

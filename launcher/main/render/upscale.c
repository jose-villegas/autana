#include "render/upscale.h"

#include <assert.h>
#include <stddef.h>

#include "render/code_layout.h"

static uint16_t
nearest_source(int destination, int destination_size, int source_size) {
    if (destination_size == 1) {
        return 0;
    }
    const int numerator = destination * (source_size - 1);
    return (uint16_t)((numerator + ((destination_size - 1) / 2)) / (destination_size - 1));
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
upscale_integer_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                     uint16_t* destination, int first_row, int row_count) {
    for (int y = first_row; y < first_row + row_count; y++) {
        const uint16_t* input = source + ((size_t)(y / scale->vertical_factor) * scale->source_width);
        const uint16_t* input_depth =
            depth == NULL ? NULL : depth + ((size_t)(y / scale->vertical_factor) * scale->source_width);
        uint16_t* output = destination + ((size_t)y * scale->destination_width);
        for (int x = 0; x < scale->source_width; x++) {
            const uint16_t pixel = input_depth != NULL && input_depth[x] == 0 ? clear : input[x];
            for (int repeat = 0; repeat < scale->horizontal_factor; repeat++) {
                output[(x * scale->horizontal_factor) + repeat] = pixel;
            }
        }
    }
}

/* Each source pixel is one 32-bit store per destination row, and a source
 * row read once feeds both of its rows when the range holds them. */
static void
upscale_double_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                    uint16_t* destination, int first_row, int row_count) {
    const int end = first_row + row_count;
    for (int y = first_row; y < end;) {
        const size_t row = (size_t)(y / 2) * scale->source_width;
        const uint16_t* input = source + row;
        const uint16_t* input_depth = depth == NULL ? NULL : depth + row;
        uint32_t* top = (uint32_t*)(destination + ((size_t)y * scale->destination_width));
        uint32_t* bottom = top + scale->source_width;
        const bool both = (y % 2 == 0) && (y + 1 < end);
        for (int x = 0; x < scale->source_width; x++) {
            const uint16_t pixel = input_depth != NULL && input_depth[x] == 0 ? clear : input[x];
            const uint32_t pair = ((uint32_t)pixel << 16) | pixel;
            top[x] = pair;
            if (both) {
                bottom[x] = pair;
            }
        }
        y += both ? 2 : 1;
    }
}

static void
upscale_mapped_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                    uint16_t* destination, int first_row, int row_count) {
    for (int y = first_row; y < first_row + row_count; y++) {
        const uint16_t* input = source + ((size_t)scale->rows[y] * scale->source_width);
        const uint16_t* input_depth = depth == NULL ? NULL : depth + ((size_t)scale->rows[y] * scale->source_width);
        uint16_t* output = destination + ((size_t)y * scale->destination_width);
        for (int x = 0; x < scale->destination_width; x++) {
            const int source_x = scale->columns[x];
            output[x] = input_depth != NULL && input_depth[source_x] == 0 ? clear : input[source_x];
        }
    }
}

RENDER_ENTRY_OFFSET(4) void
upscale_rows(const upscale_t* scale, const gfx_render_target_t* picture, uint16_t clear, uint16_t* destination,
             int first_row, int row_count) {
    assert(scale != NULL);
    assert(picture != NULL);
    const uint16_t* source = gfx_render_target_color(picture, picture->row0);
    const uint16_t* depth =
        picture->count > GFX_ATTACHMENT_DEPTH ? gfx_render_target_depth(picture, picture->row0) : NULL;
    assert(destination != NULL);
    assert(first_row >= 0);
    assert(row_count >= 0);
    assert(first_row + row_count <= scale->destination_height);
    if (scale->integer && scale->horizontal_factor == 2) {
        upscale_double_rows(scale, source, depth, clear, destination, first_row, row_count);
    } else if (scale->integer) {
        upscale_integer_rows(scale, source, depth, clear, destination, first_row, row_count);
    } else {
        upscale_mapped_rows(scale, source, depth, clear, destination, first_row, row_count);
    }
}

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

/* The width decides: kept (on an even width, for 32-bit stores) or doubled
 * takes any row map; otherwise both axes by one integer, or the maps. */
static upscale_path_t
choose_path(int source_width, int source_height, int destination_width, int destination_height) {
    if (destination_width == source_width && destination_width % 2 == 0) {
        return UPSCALE_KEPT;
    }
    if (destination_width == 2 * source_width) {
        return UPSCALE_DOUBLED;
    }
    const int factor = destination_width / source_width;
    const bool integer = destination_width % source_width == 0 && destination_height % source_height == 0
                         && factor == destination_height / source_height;
    return integer ? UPSCALE_BLOCKS : UPSCALE_MAPPED;
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
    scale->factor = destination_width / source_width;
    scale->path = choose_path(source_width, source_height, destination_width, destination_height);
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

/* A source pixel as shown: `clear` where nothing was drawn. */
static inline uint16_t
shown(const uint16_t* input, const uint16_t* input_depth, int x, uint16_t clear) {
    return input_depth != NULL && input_depth[x] == 0 ? clear : input[x];
}

static void
upscale_block_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                   uint16_t* destination, int first_row, int row_count) {
    for (int y = first_row; y < first_row + row_count; y++) {
        const size_t row = (size_t)(y / scale->factor) * scale->source_width;
        const uint16_t* input_depth = depth == NULL ? NULL : depth + row;
        uint16_t* output = destination + ((size_t)y * scale->destination_width);
        for (int x = 0; x < scale->source_width; x++) {
            const uint16_t pixel = shown(source + row, input_depth, x, clear);
            for (int repeat = 0; repeat < scale->factor; repeat++) {
                output[(x * scale->factor) + repeat] = pixel;
            }
        }
    }
}

/* UPSCALE_KEPT and UPSCALE_DOUBLED store 32 bits at a time: two
 * neighbouring source pixels when the width is kept, one twice when it is
 * doubled. A source row read once feeds the next destination row too when
 * the row map repeats it inside the range. Inlined into each path, so
 * neither tests `doubled` per pixel. */
static inline __attribute__((always_inline)) void
paired_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
            uint16_t* destination, int first_row, int row_count, bool doubled) {
    const int end = first_row + row_count;
    const int stores = scale->destination_width / 2;
    for (int y = first_row; y < end;) {
        const size_t row = (size_t)scale->rows[y] * scale->source_width;
        const uint16_t* input = source + row;
        const uint16_t* input_depth = depth == NULL ? NULL : depth + row;
        uint32_t* top = (uint32_t*)(destination + ((size_t)y * scale->destination_width));
        uint32_t* bottom = top + stores;
        const bool both = y + 1 < end && scale->rows[y + 1] == scale->rows[y];
        for (int x = 0; x < stores; x++) {
            const uint32_t pair = doubled ? 0x10001U * shown(input, input_depth, x, clear)
                                          : shown(input, input_depth, 2 * x, clear)
                                                | ((uint32_t)shown(input, input_depth, (2 * x) + 1, clear) << 16);
            top[x] = pair;
            if (both) {
                bottom[x] = pair;
            }
        }
        y += both ? 2 : 1;
    }
}

static void
upscale_kept_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                  uint16_t* destination, int first_row, int row_count) {
    paired_rows(scale, source, depth, clear, destination, first_row, row_count, false);
}

static void
upscale_doubled_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                     uint16_t* destination, int first_row, int row_count) {
    paired_rows(scale, source, depth, clear, destination, first_row, row_count, true);
}

static void
upscale_mapped_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                    uint16_t* destination, int first_row, int row_count) {
    for (int y = first_row; y < first_row + row_count; y++) {
        const size_t row = (size_t)scale->rows[y] * scale->source_width;
        const uint16_t* input_depth = depth == NULL ? NULL : depth + row;
        uint16_t* output = destination + ((size_t)y * scale->destination_width);
        for (int x = 0; x < scale->destination_width; x++) {
            output[x] = shown(source + row, input_depth, scale->columns[x], clear);
        }
    }
}

RENDER_ENTRY_OFFSET(4) void
upscale_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
             uint16_t* destination, int first_row, int row_count) {
    assert(scale != NULL);
    assert(source != NULL);
    assert(destination != NULL);
    assert(first_row >= 0);
    assert(row_count >= 0);
    assert(first_row + row_count <= scale->destination_height);
    switch (scale->path) {
        case UPSCALE_KEPT: upscale_kept_rows(scale, source, depth, clear, destination, first_row, row_count); break;
        case UPSCALE_DOUBLED:
            upscale_doubled_rows(scale, source, depth, clear, destination, first_row, row_count);
            break;
        case UPSCALE_BLOCKS: upscale_block_rows(scale, source, depth, clear, destination, first_row, row_count); break;
        case UPSCALE_MAPPED: upscale_mapped_rows(scale, source, depth, clear, destination, first_row, row_count); break;
    }
}

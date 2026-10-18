#include "render/upscale.h"

#include <assert.h>
#include <stddef.h>

#include "gfx/target/gfx_target.h"
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
    scale->scanline_strength = 0;
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
    const int run = destination_height / source_height;
    scale->scanline_run = 0;
    if (destination_height % source_height == 0 && run % 2 == 0) {
        scale->scanline_run = run;
        for (int y = 0; scale->path != UPSCALE_BLOCKS && y < destination_height; y++) {
            if (rows[y] != y / run) {
                scale->scanline_run = 0;
                break;
            }
        }
    }
    return true;
}

static int
scanline_run(const upscale_t* scale) {
    return scale->scanline_strength == 0 ? 0 : scale->scanline_run;
}

static uint16_t
scanline_pixel(const upscale_t* scale, uint16_t pixel, int y) {
    const int run = scanline_run(scale);
    if (run == 0 || y % run < run / 2) {
        return pixel;
    }
    return gfx_color_scale(pixel, UPSCALE_SCANLINE_UNIT - scale->scanline_strength);
}

static void
upscale_block_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                   uint16_t* destination, int first_row, int row_count) {
    for (int y = first_row; y < first_row + row_count; y++) {
        const size_t row = (size_t)(y / scale->factor) * scale->source_width;
        const uint16_t* input_depth = depth == NULL ? NULL : depth + row;
        uint16_t* output = destination + ((size_t)y * scale->destination_width);
        for (int x = 0; x < scale->source_width; x++) {
            const uint16_t pixel =
                scanline_pixel(scale, gfx_target_shown_pixel(source + row, input_depth, x, clear), y);
            for (int repeat = 0; repeat < scale->factor; repeat++) {
                output[(x * scale->factor) + repeat] = pixel;
            }
        }
    }
}

static void
upscale_paired_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                    uint16_t* destination, int first_row, int row_count, bool doubled) {
    const gfx_target_t target = {destination, 0, scale->destination_height, scale->destination_width};
    if (scanline_run(scale) == 0) {
        gfx_target_paired_rows(target, source, depth, clear, scale->source_width, scale->rows, first_row, row_count,
                               doubled);
    } else {
        gfx_target_paired_rows_shaded(target, source, depth, clear, scale->source_width, scale->rows, first_row,
                                      row_count, doubled, scale->scanline_run,
                                      UPSCALE_SCANLINE_UNIT - scale->scanline_strength);
    }
}

static void
upscale_mapped_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
                    uint16_t* destination, int first_row, int row_count) {
    for (int y = first_row; y < first_row + row_count; y++) {
        const size_t row = (size_t)scale->rows[y] * scale->source_width;
        const uint16_t* input_depth = depth == NULL ? NULL : depth + row;
        uint16_t* output = destination + ((size_t)y * scale->destination_width);
        for (int x = 0; x < scale->destination_width; x++) {
            output[x] =
                scanline_pixel(scale, gfx_target_shown_pixel(source + row, input_depth, scale->columns[x], clear), y);
        }
    }
}

RENDER_IRAM_ATTR void
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
    switch (scale->path) {
        case UPSCALE_KEPT:
            upscale_paired_rows(scale, source, depth, clear, destination, first_row, row_count, false);
            break;
        case UPSCALE_DOUBLED:
            upscale_paired_rows(scale, source, depth, clear, destination, first_row, row_count, true);
            break;
        case UPSCALE_BLOCKS: upscale_block_rows(scale, source, depth, clear, destination, first_row, row_count); break;
        case UPSCALE_MAPPED: upscale_mapped_rows(scale, source, depth, clear, destination, first_row, row_count); break;
    }
}

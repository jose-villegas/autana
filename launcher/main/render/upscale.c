#include "render/upscale.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

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

#if defined(DEVICE_BUILD) && defined(__XTENSA__)
static bool
pie_double_aligned(const uint16_t* input, const uint32_t* output) {
    return ((uintptr_t)input & 15) == 0 && ((uintptr_t)output & 15) == 0;
}

static void
pie_double_blocks(const uint16_t* input, uint32_t* top, uint32_t* bottom, int blocks) {
    __asm__ volatile("loopgtz %[blocks], 0f\n"
                     "ee.vld.128.ip q0, %[input], 16\n"
                     "mv.qr q1, q0\n"
                     "ee.vzip.16 q0, q1\n"
                     "ee.vst.128.ip q0, %[top], 16\n"
                     "ee.vst.128.ip q1, %[top], 16\n"
                     "ee.vst.128.ip q0, %[bottom], 16\n"
                     "ee.vst.128.ip q1, %[bottom], 16\n"
                     "0:\n"
                     : [input] "+r"(input), [top] "+r"(top), [bottom] "+r"(bottom)
                     : [blocks] "r"(blocks)
                     : "memory");
}

static void
upscale_double_rows_pie(const upscale_t* scale, const uint16_t* source, uint16_t* destination, int first_row,
                        int row_count) {
    const int end = first_row + row_count;
    for (int y = first_row; y < end;) {
        const size_t row = (size_t)(y / 2) * scale->source_width;
        const uint16_t* input = source + row;
        uint32_t* top = (uint32_t*)(destination + ((size_t)y * scale->destination_width));
        uint32_t* bottom = top + scale->source_width;
        const bool both = (y % 2 == 0) && (y + 1 < end);
        const int head = (int)(((16 - ((uintptr_t)top & 15)) & 15) / sizeof(*top));
        const int vector_pixels = scale->source_width - head;
        const int blocks = vector_pixels / 8;
        if (!both || head > scale->source_width || blocks == 0 || !pie_double_aligned(input + head, top + head)) {
            upscale_double_rows(scale, source, NULL, 0, destination, y, 1);
            y++;
            continue;
        }
        for (int x = 0; x < head; x++) {
            const uint32_t pair = ((uint32_t)input[x] << 16) | input[x];
            top[x] = pair;
            bottom[x] = pair;
        }
        pie_double_blocks(input + head, top + head, bottom + head, blocks);
        for (int x = head + blocks * 8; x < scale->source_width; x++) {
            const uint32_t pair = ((uint32_t)input[x] << 16) | input[x];
            top[x] = pair;
            bottom[x] = pair;
        }
        y += 2;
    }
}
#endif

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

void
upscale_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
             uint16_t* destination, int first_row, int row_count) {
    assert(scale != NULL);
    assert(source != NULL);
    assert(destination != NULL);
    assert(first_row >= 0);
    assert(row_count >= 0);
    assert(first_row + row_count <= scale->destination_height);
    if (scale->integer && scale->horizontal_factor == 2) {
#if defined(DEVICE_BUILD) && defined(__XTENSA__)
        if (depth == NULL) {
            upscale_double_rows_pie(scale, source, destination, first_row, row_count);
            return;
        }
#endif
        upscale_double_rows(scale, source, depth, clear, destination, first_row, row_count);
    } else if (scale->integer) {
        upscale_integer_rows(scale, source, depth, clear, destination, first_row, row_count);
    } else {
        upscale_mapped_rows(scale, source, depth, clear, destination, first_row, row_count);
    }
}

#if defined(DEVICE_BUILD) && defined(CONFIG_LAUNCHER_SELFTEST) && CONFIG_LAUNCHER_SELFTEST
void
upscale_rows_c(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, uint16_t clear,
               uint16_t* destination, int first_row, int row_count) {
    if (scale->integer && scale->horizontal_factor == 2) {
        upscale_double_rows(scale, source, depth, clear, destination, first_row, row_count);
    } else if (scale->integer) {
        upscale_integer_rows(scale, source, depth, clear, destination, first_row, row_count);
    } else {
        upscale_mapped_rows(scale, source, depth, clear, destination, first_row, row_count);
    }
}
#endif

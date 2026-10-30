#include "render/upscale.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

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
    if ((scale->path == UPSCALE_KEPT || scale->path == UPSCALE_DOUBLED) && destination_height % source_height == 0
        && run % 2 == 0) {
        scale->scanline_run = run;
        for (int y = 0; y < destination_height; y++) {
            if (rows[y] != y / run) {
                scale->scanline_run = 0;
                break;
            }
        }
    }
    return true;
}

static void
upscale_block_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, size_t pitch, uint16_t clear,
                   uint16_t* destination, int first_row, int row_count) {
    for (int y = first_row; y < first_row + row_count; y++) {
        const size_t source_y = y / scale->factor;
        const uint16_t* input = (const uint16_t*)((const char*)source + (source_y * pitch));
        const uint16_t* input_depth = depth == NULL ? NULL : (const uint16_t*)((const char*)depth + (source_y * pitch));
        uint16_t* output = destination + ((size_t)y * scale->destination_width);
        for (int x = 0; x < scale->source_width; x++) {
            const uint16_t pixel = gfx_target_shown_pixel(input, input_depth, x, clear);
            for (int repeat = 0; repeat < scale->factor; repeat++) {
                output[(x * scale->factor) + repeat] = pixel;
            }
        }
    }
}

static void
upscale_mapped_rows(const upscale_t* scale, const uint16_t* source, const uint16_t* depth, size_t pitch, uint16_t clear,
                    uint16_t* destination, int first_row, int row_count) {
    for (int y = first_row; y < first_row + row_count; y++) {
        const size_t source_y = scale->rows[y];
        const uint16_t* input = (const uint16_t*)((const char*)source + (source_y * pitch));
        const uint16_t* input_depth = depth == NULL ? NULL : (const uint16_t*)((const char*)depth + (source_y * pitch));
        uint16_t* output = destination + ((size_t)y * scale->destination_width);
        for (int x = 0; x < scale->destination_width; x++) {
            output[x] = gfx_target_shown_pixel(input, input_depth, scale->columns[x], clear);
        }
    }
}

/* Shared validation stays in flash to keep assertion paths out of the draw loop's IRAM budget. */
static __attribute__((noinline)) void
check_rows(const upscale_t* scale, const gfx_target_t* picture, const uint16_t* destination, int first_row,
           int row_count) {
    assert(scale != NULL);
    assert(picture != NULL);
    assert(destination != NULL);
    assert(first_row >= 0);
    assert(row_count >= 0);
    assert(first_row + row_count <= scale->destination_height);
}

RENDER_IRAM_ATTR void
upscale_rows(const upscale_t* scale, const gfx_target_t* picture, uint16_t clear, uint16_t* destination, int first_row,
             int row_count) {
    check_rows(scale, picture, destination, first_row, row_count);
    GFX_TARGET_STORAGE(1)
    storage = GFX_TARGET_COLOR_INIT(destination, scale->destination_width, scale->destination_height);
    gfx_target_t* target = &storage.target;
    const uint16_t* source = picture->attachment[0].pixels;
    const uint16_t* depth =
        picture->count > GFX_ATTACHMENT_DEPTH ? picture->attachment[GFX_ATTACHMENT_DEPTH].pixels : NULL;
    const size_t pitch = picture->attachment[0].pitch;
    switch (scale->path) {
        case UPSCALE_KEPT:
            gfx_target_paired_rows(target, picture, clear, scale->rows, first_row, row_count, false);
            break;
        case UPSCALE_DOUBLED:
            gfx_target_paired_rows(target, picture, clear, scale->rows, first_row, row_count, true);
            break;
        case UPSCALE_BLOCKS:
            upscale_block_rows(scale, source, depth, pitch, clear, destination, first_row, row_count);
            break;
        case UPSCALE_MAPPED:
            upscale_mapped_rows(scale, source, depth, pitch, clear, destination, first_row, row_count);
            break;
    }
}

void
upscale_init_blends(upscale_t* scale, uint16_t* blends) {
    /* Destination row y's centre falls on source row (2y + 1) * source / (2 * destination) - 1/2. */
    const int span = 2 * scale->destination_height;
    for (int y = 0; y < scale->destination_height; y++) {
        const int at = ((2 * y) + 1) * scale->source_height - scale->destination_height;
        int row = at <= 0 ? 0 : at / span;
        int share = at <= 0 ? 0 : (((at % span) * RGB565_SCALE_UNIT) + (span / 2)) / span;
        if (share == RGB565_SCALE_UNIT) {
            row++;
            share = 0;
        }
        if (row >= scale->source_height - 1) {
            row = scale->source_height - 1;
            share = 0;
        }
        blends[y] = UPSCALE_BLEND(row, share);
    }
    scale->blends = blends;
}

static int
blend_row(uint16_t blend) {
    return blend / RGB565_SCALE_UNIT;
}

static unsigned
blend_share(uint16_t blend) {
    return blend % RGB565_SCALE_UNIT;
}

/* The source row a blend leans on most: the one a depth test stands for. */
static int
blend_nearest_row(uint16_t blend) {
    return blend_row(blend) + (blend_share(blend) >= RGB565_SCALE_UNIT / 2 ? 1 : 0);
}

static const uint16_t*
source_row(const uint16_t* pixels, size_t pitch, int row) {
    return (const uint16_t*)((const char*)pixels + ((size_t)row * pitch));
}

static void
mark_seams(const uint16_t* here, const uint16_t* below, int width, uint8_t* seams) {
    unsigned bits = 0;
    for (int x = 0; x < width; x++) {
        const bool right = x + 1 < width && !upscale_same_surface(here[x], here[x + 1]);
        const bool down = below != NULL && !upscale_same_surface(here[x], below[x]);
        bits |= (unsigned)(right || down) << (x % GFX_SEAM_BITS);
        if (x % GFX_SEAM_BITS == GFX_SEAM_BITS - 1 || x + 1 == width) {
            seams[x / GFX_SEAM_BITS] = (uint8_t)bits;
            bits = 0;
        }
    }
}

/* Destination row `y` from its blend's two source rows: a copy on a source
 * row, else the blend within a surface and the nearer row across a seam. */
static void
surface_row(const upscale_t* scale, const uint16_t* color, const uint16_t* depth, size_t pitch, uint16_t clear, int y,
            uint16_t* output) {
    const uint16_t blend = scale->blends[y];
    const unsigned share = blend_share(blend);
    const int lower = blend_row(blend);
    const uint16_t* lower_color = source_row(color, pitch, lower);
    const uint16_t* lower_depth = source_row(depth, pitch, lower);
    if (share == 0) {
        for (int x = 0; x < scale->destination_width; x++) {
            output[x] = gfx_target_shown_pixel(lower_color, lower_depth, x, clear);
        }
        return;
    }
    const uint16_t* upper_color = source_row(color, pitch, lower + 1);
    const uint16_t* upper_depth = source_row(depth, pitch, lower + 1);
    const bool upper_nearer = blend_nearest_row(blend) == lower + 1;
    for (int x = 0; x < scale->destination_width; x++) {
        const uint16_t a = gfx_target_shown_pixel(lower_color, lower_depth, x, clear);
        const uint16_t b = gfx_target_shown_pixel(upper_color, upper_depth, x, clear);
        output[x] = upscale_same_surface(lower_depth[x], upper_depth[x]) ? rgb565be_blend(a, b, share)
                    : upper_nearer                                       ? b
                                                                         : a;
    }
}

static void
surface_rows(const upscale_t* scale, const uint16_t* color, const uint16_t* depth, size_t pitch, uint16_t clear,
             uint16_t* destination, uint8_t* seams, int first_row, int row_count) {
    const int width = scale->destination_width;
    for (int y = first_row; y < first_row + row_count; y++) {
        surface_row(scale, color, depth, pitch, clear, y, destination + ((size_t)y * width));
        if (seams != NULL) {
            const uint16_t* here = source_row(depth, pitch, blend_nearest_row(scale->blends[y]));
            const uint16_t* below = y + 1 < scale->destination_height
                                        ? source_row(depth, pitch, blend_nearest_row(scale->blends[y + 1]))
                                        : NULL;
            mark_seams(here, below, width, seams + ((size_t)y * GFX_SEAM_PITCH(width)));
        }
    }
}

void
upscale_rows_filtered(const upscale_t* scale, const gfx_target_t* picture, uint16_t clear, uint16_t* destination,
                      uint8_t* seams, int first_row, int row_count) {
    check_rows(scale, picture, destination, first_row, row_count);
    const uint16_t* depth =
        picture->count > GFX_ATTACHMENT_DEPTH ? picture->attachment[GFX_ATTACHMENT_DEPTH].pixels : NULL;
    if (scale->filter != UPSCALE_FILTER_SURFACE || scale->path != UPSCALE_KEPT || depth == NULL) {
        upscale_rows(scale, picture, clear, destination, first_row, row_count);
        if (seams != NULL) {
            /* Nothing known about surfaces: every pixel is its own. */
            memset(seams + ((size_t)first_row * GFX_SEAM_PITCH(scale->destination_width)), UINT8_MAX,
                   GFX_SEAM_BYTES(scale->destination_width, row_count));
        }
        return;
    }
    surface_rows(scale, picture->attachment[0].pixels, depth, picture->attachment[0].pitch, clear, destination, seams,
                 first_row, row_count);
}

/* Scanline dispatch stays in flash to preserve the default copy's IRAM budget. */
void
upscale_rows_scanlined(const upscale_t* scale, const gfx_target_t* picture, uint16_t clear, uint16_t* destination,
                       int first_row, int row_count) {
    check_rows(scale, picture, destination, first_row, row_count);
    if (scale->scanline_run == 0 || (scale->path != UPSCALE_KEPT && scale->path != UPSCALE_DOUBLED)) {
        upscale_rows(scale, picture, clear, destination, first_row, row_count);
        return;
    }
    GFX_TARGET_STORAGE(1)
    storage = GFX_TARGET_COLOR_INIT(destination, scale->destination_width, scale->destination_height);
    gfx_target_t* target = &storage.target;
    const uint16_t* depth =
        picture->count > GFX_ATTACHMENT_DEPTH ? picture->attachment[GFX_ATTACHMENT_DEPTH].pixels : NULL;
    gfx_target_paired_rows_shaded(target, &picture->attachment[GFX_ATTACHMENT_COLOR], depth, clear, scale->rows,
                                  first_row, row_count, scale->path == UPSCALE_DOUBLED, scale->scanline_run,
                                  UPSCALE_SCANLINE_KEEP);
}

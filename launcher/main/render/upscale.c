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
        const int at = (((2 * y) + 1) * scale->source_height) - scale->destination_height;
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

/* Bit i set where a[i] and b[i], i < count, are not one surface. */
static unsigned
surface_steps(const uint16_t* a, const uint16_t* b, int count) {
    unsigned bits = 0;
    for (int i = 0; i < count; i++) {
        bits |= (unsigned)!upscale_same_surface(a[i], b[i]) << i;
    }
    return bits;
}

/* One destination row's seams, a byte at a time: `right`, the seams `here`
 * has against its right neighbours where already known, else found; then
 * `here` against `below`, unless NULL. */
static void
mark_seams(const uint16_t* here, const uint16_t* below, int width, const uint8_t* right, uint8_t* seams) {
    for (int byte = 0; byte < GFX_SEAM_PITCH(width); byte++) {
        const int first = byte * GFX_SEAM_BITS;
        const int count = width - first < GFX_SEAM_BITS ? width - first : GFX_SEAM_BITS;
        const int across = first + count == width ? count - 1 : count; /* the last pixel has none right */
        unsigned bits = right != NULL ? right[byte] : surface_steps(here + first, here + first + 1, across);
        if (below != NULL) {
            bits |= surface_steps(here + first, below + first, count);
        }
        seams[byte] = (uint8_t)bits;
    }
}

/* The last seam row that holds only its source row's right seams. */
typedef struct {
    int row;
    const uint8_t* seams;
} known_seams_t;

/* Destination row `y`'s seams. Rows that stand for one source row share its
 * right seams: the first with nothing below to add keeps them for the rest. */
static void
row_seams(const upscale_t* scale, const uint16_t* depth, size_t pitch, int y, uint8_t* seams, known_seams_t* known) {
    const int here_row = blend_nearest_row(scale->blends[y]);
    const int below_row = y + 1 < scale->destination_height ? blend_nearest_row(scale->blends[y + 1]) : here_row;
    const uint8_t* right = here_row == known->row ? known->seams : NULL;
    mark_seams(source_row(depth, pitch, here_row), below_row != here_row ? source_row(depth, pitch, below_row) : NULL,
               scale->destination_width, right, seams);
    if (right == NULL && below_row == here_row) {
        *known = (known_seams_t){.row = here_row, .seams = seams};
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
    known_seams_t known = {.row = -1, .seams = NULL};
    for (int y = first_row; y < first_row + row_count; y++) {
        surface_row(scale, color, depth, pitch, clear, y, destination + ((size_t)y * width));
        if (seams != NULL) {
            row_seams(scale, depth, pitch, y, seams + ((size_t)y * GFX_SEAM_PITCH(width)), &known);
        }
    }
}

/* Bit i set where Scale2x fills a corner of pixel `first` + i, i < count,
 * from its neighbour: up A, right B, left C and down D, an edge pixel being
 * its own neighbour there. A pair is equal when its depths are one surface. */
typedef struct {
    unsigned top_left, top_right, bottom_left, bottom_right;
} corner_bits_t;

static corner_bits_t
corner_bits(const uint16_t* up, const uint16_t* here, const uint16_t* down, int width, int first, int count) {
    unsigned ca = 0, ab = 0, cd = 0, bd = 0;
    for (int i = 0; i < count; i++) {
        const int x = first + i;
        const uint16_t a = up[x];
        const uint16_t b = here[x + 1 < width ? x + 1 : x];
        const uint16_t c = here[x > 0 ? x - 1 : x];
        const uint16_t d = down[x];
        ca |= (unsigned)upscale_same_surface(c, a) << i;
        ab |= (unsigned)upscale_same_surface(a, b) << i;
        cd |= (unsigned)upscale_same_surface(c, d) << i;
        bd |= (unsigned)upscale_same_surface(b, d) << i;
    }
    return (corner_bits_t){.top_left = ca & ~cd & ~ab,
                           .top_right = ab & ~ca & ~bd,
                           .bottom_left = cd & ~bd & ~ca,
                           .bottom_right = bd & ~ab & ~cd};
}

/* Destination row `y`'s corners, from the depths of the rows around it as
 * the nearest copy lays them out. A row inside one source row's run has its
 * own surfaces above and below: no corner. */
static void
row_corners(const upscale_t* scale, const uint16_t* depth, size_t pitch, int y, uint8_t* hints) {
    const int width = scale->destination_width;
    const int height = scale->destination_height;
    const int pitch_bytes = GFX_SEAM_PITCH(width);
    uint8_t* planes[GFX_HINT_PLANES];
    for (int corner = 0; corner < GFX_HINT_PLANES; corner++) {
        planes[corner] = GFX_HINT_PLANE(hints, width, height, corner) + ((size_t)y * pitch_bytes);
    }
    const int here_row = scale->rows[y];
    const int up_row = y > 0 ? scale->rows[y - 1] : here_row;
    const int down_row = y + 1 < height ? scale->rows[y + 1] : here_row;
    if (up_row == here_row && down_row == here_row) {
        for (int corner = 0; corner < GFX_HINT_PLANES; corner++) {
            memset(planes[corner], 0, pitch_bytes);
        }
        return;
    }
    const uint16_t* up = source_row(depth, pitch, up_row);
    const uint16_t* here = source_row(depth, pitch, here_row);
    const uint16_t* down = source_row(depth, pitch, down_row);
    for (int byte = 0; byte < pitch_bytes; byte++) {
        const int first = byte * GFX_SEAM_BITS;
        const int count = width - first < GFX_SEAM_BITS ? width - first : GFX_SEAM_BITS;
        const corner_bits_t bits = corner_bits(up, here, down, width, first, count);
        planes[GFX_HINT_CORNER(0, 0)][byte] = (uint8_t)bits.top_left;
        planes[GFX_HINT_CORNER(0, 1)][byte] = (uint8_t)bits.top_right;
        planes[GFX_HINT_CORNER(1, 0)][byte] = (uint8_t)bits.bottom_left;
        planes[GFX_HINT_CORNER(1, 1)][byte] = (uint8_t)bits.bottom_right;
    }
    /* Off the picture's sides the neighbour is the pixel itself: nothing to take. */
    const uint8_t last = (uint8_t)~(1U << ((width - 1) % GFX_SEAM_BITS));
    for (int row = 0; row < 2; row++) {
        planes[GFX_HINT_CORNER(row, 0)][0] &= (uint8_t)~1U;
        planes[GFX_HINT_CORNER(row, 1)][pitch_bytes - 1] &= last;
    }
}

/* Rows [first_row, first_row + row_count) with nothing known about surfaces:
 * every pixel its own, so a seam everywhere and no corner. */
static void
mark_unknown(const upscale_t* scale, uint8_t* hints, int first_row, int row_count) {
    const int width = scale->destination_width;
    const size_t at = (size_t)first_row * GFX_SEAM_PITCH(width);
    if (scale->filter != UPSCALE_FILTER_EDGES) {
        memset(hints + at, UINT8_MAX, GFX_SEAM_BYTES(width, row_count));
        return;
    }
    for (int corner = 0; corner < GFX_HINT_PLANES; corner++) {
        memset(GFX_HINT_PLANE(hints, width, scale->destination_height, corner) + at, 0,
               GFX_SEAM_BYTES(width, row_count));
    }
}

void
upscale_rows_filtered(const upscale_t* scale, const gfx_target_t* picture, uint16_t clear, uint16_t* destination,
                      uint8_t* hints, int first_row, int row_count) {
    check_rows(scale, picture, destination, first_row, row_count);
    const uint16_t* depth =
        picture->count > GFX_ATTACHMENT_DEPTH ? picture->attachment[GFX_ATTACHMENT_DEPTH].pixels : NULL;
    if (scale->filter == UPSCALE_FILTER_NEAREST || scale->path != UPSCALE_KEPT || depth == NULL) {
        upscale_rows(scale, picture, clear, destination, first_row, row_count);
        if (hints != NULL) {
            mark_unknown(scale, hints, first_row, row_count);
        }
        return;
    }
    const size_t pitch = picture->attachment[0].pitch;
    if (scale->filter == UPSCALE_FILTER_EDGES) {
        upscale_rows(scale, picture, clear, destination, first_row, row_count);
        for (int y = first_row; hints != NULL && y < first_row + row_count; y++) {
            row_corners(scale, depth, pitch, y, hints);
        }
        return;
    }
    surface_rows(scale, picture->attachment[0].pixels, depth, pitch, clear, destination, hints, first_row, row_count);
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

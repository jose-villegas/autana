#include "core/memory.h"
#include "gfx/present/gfx_internal.h"
#include "gfx/present/gfx_present_guard.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"

static const char* TAG = "gfx";
#endif

/* GFX_LAYOUT_INDEXED's own state: the app writes indices,
 * run_present_strips() (gfx_present.c) expands them through whichever LUT is
 * installed. Not a gfx_target.h render target: no drawing primitive writes
 * through it. */
static uint8_t* indexed_image;
static int indexed_grid_w, indexed_grid_h, indexed_cell_size;
static gfx_color_t indexed_lut256[GFX_INDEXED_PALETTE_SIZE];
static bool indexed_dither16_on;

/* Lever 2: which of gfx_dither_mode_t's five is installed for 16-colour
 * mode, meaningless while indexed_dither16_on is false (256 mode keeps
 * its own plain LUT above, no dither concept at all). One table per mode,
 * not a shared buffer: gfx_indexed_set_dither() only ever overwrites the
 * one an app's own mode switch actually asks for. */
static gfx_dither_mode_t indexed_dither_mode = GFX_DITHER_PIXEL_BAYER4;
static gfx_color_t indexed_dither_none_lut[GFX_INDEXED_PALETTE_SIZE];
static gfx_color_t indexed_dither_cell_checker[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_CELL_CHECKER_PHASES];
static gfx_color_t indexed_dither_cell_bayer2[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_CELL_BAYER2_PHASES];
static gfx_color_t indexed_dither_pixel_checker2[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_CHECKER2_ROW_PHASES
                                                 * GFX_INDEXED_CHECKER2_CHUNK_PX];
static gfx_color_t indexed_dither16_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES];

static const gfx_color_t*
indexed_active_table(void) {
    if (!indexed_dither16_on) {
        return indexed_lut256;
    }
    switch (indexed_dither_mode) {
        case GFX_DITHER_NONE: return indexed_dither_none_lut;
        case GFX_DITHER_CELL_CHECKER: return indexed_dither_cell_checker;
        case GFX_DITHER_CELL_BAYER2: return indexed_dither_cell_bayer2;
        case GFX_DITHER_PIXEL_CHECKER2: return indexed_dither_pixel_checker2;
        case GFX_DITHER_PIXEL_BAYER4:
        case GFX_DITHER_MODE_COUNT:
        default: return indexed_dither16_rgb;
    }
}

gfx_indexed_frame_t
indexed_frame(void) {
    return (gfx_indexed_frame_t){
        .image = indexed_image,
        .grid_w = indexed_grid_w,
        .grid_h = indexed_grid_h,
        .cell_size = indexed_cell_size,
        .dither16_on = indexed_dither16_on,
        .dither_mode = indexed_dither_mode,
        .table = indexed_active_table(),
    };
}

#ifdef ESP_PLATFORM
static bool
alloc_indexed_image(int grid_w, int grid_h) {
    const size_t bytes = (size_t)grid_w * (size_t)grid_h;
    indexed_image = memory_alloc(bytes, MEMORY_INTERNAL);
    if (indexed_image == NULL) {
        ESP_LOGE(TAG, "Could not allocate %u byte index image", (unsigned)bytes);
        return false;
    }
    return true;
}

static void
free_indexed_image(void) {
    memory_free(indexed_image);
    indexed_image = NULL;
}
#else
static bool
alloc_indexed_image(int grid_w, int grid_h) {
    indexed_image = malloc((size_t)grid_w * (size_t)grid_h);
    return indexed_image != NULL;
}

static void
free_indexed_image(void) {
    free(indexed_image);
    indexed_image = NULL;
}
#endif

bool
indexed_begin(const gfx_mode_t* granted) {
    if (!alloc_indexed_image(granted->index_grid_w, granted->index_grid_h)) {
        return false;
    }
    indexed_grid_w = granted->index_grid_w;
    indexed_grid_h = granted->index_grid_h;
    indexed_cell_size = granted->cell_size;
    indexed_dither16_on = false;
    return true;
}

void
indexed_end(void) {
    free_indexed_image();
}

image_t
gfx_indexed_image(void) {
    GFX_PRESENT_GUARD();
    return (image_t){.pixels = indexed_image,
                     .format = IMAGE_INDEXED8,
                     .width = (uint16_t)indexed_grid_w,
                     .height = (uint16_t)indexed_grid_h,
                     .pitch = (uint32_t)indexed_grid_w};
}

void
gfx_indexed_set_palette(const palette_t* palette) {
    GFX_PRESENT_GUARD();
    assert(palette->count == GFX_INDEXED_PALETTE_SIZE);
    memcpy(indexed_lut256, palette->entries, sizeof indexed_lut256);
}

void
gfx_indexed_set_lut16(const gfx_color_t dither16_rgb[GFX_INDEXED_PALETTE_SIZE * GFX_INDEXED_DITHER16_PHASES]) {
    GFX_PRESENT_GUARD();
    memcpy(indexed_dither16_rgb, dither16_rgb, sizeof indexed_dither16_rgb);
}

void
gfx_indexed_set_dither16(bool enabled) {
    GFX_PRESENT_GUARD();
    indexed_dither16_on = enabled;
}

/* Installs `table` for `mode` and selects it as the active one:
 * GFX_LAYOUT_INDEXED's own dither pattern while indexed_dither16_on is
 * true (gfx_indexed_set_dither16()); meaningless in 256 mode, which never
 * consults it. `table` must be sized for `mode`; see gfx_dither_mode_t's
 * own comment (gfx_indexed.h) for which. Present-task-only, like every
 * other indexed setter here (GFX_PRESENT_GUARD()). */
void
gfx_indexed_set_dither(gfx_dither_mode_t mode, const gfx_color_t* table) {
    GFX_PRESENT_GUARD();
    switch (mode) {
        case GFX_DITHER_NONE: memcpy(indexed_dither_none_lut, table, sizeof indexed_dither_none_lut); break;
        case GFX_DITHER_CELL_CHECKER:
            memcpy(indexed_dither_cell_checker, table, sizeof indexed_dither_cell_checker);
            break;
        case GFX_DITHER_CELL_BAYER2:
            memcpy(indexed_dither_cell_bayer2, table, sizeof indexed_dither_cell_bayer2);
            break;
        case GFX_DITHER_PIXEL_CHECKER2:
            memcpy(indexed_dither_pixel_checker2, table, sizeof indexed_dither_pixel_checker2);
            break;
        case GFX_DITHER_PIXEL_BAYER4: memcpy(indexed_dither16_rgb, table, sizeof indexed_dither16_rgb); break;
        case GFX_DITHER_MODE_COUNT: break;
    }
    indexed_dither_mode = mode;
}

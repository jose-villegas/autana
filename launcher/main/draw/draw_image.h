/*
 * draw_image: a picture as an asset-pack entry (IMAG), written by
 * launcher/tools/gfx/image_asset.py. Opening it checks the header and that
 * every row lies inside the entry; the pixels then point into the entry, so a
 * mapped pack draws with no copy and no RAM. A caller finds the entry with
 * asset_pack_find() and DRAW_IMAGE_ASSET. The layout is in
 * docs/assets/Image-Entries.md.
 *
 * No allocation and no ESP-IDF.
 */
#pragma once

#include <stdint.h>

#include "asset/asset_pack.h"
#include "draw/draw_image_format.h"
#include "gfx/target/gfx_color.h"

/* How a pixel is stored. One type holds every picture the engine draws, so a
 * new kind of picture is a new format here, not a new entry type. */
typedef enum {
    DRAW_IMAGE_RGB565 = DRAW_IMAGE_FORMAT_RGB565, /* gfx_color_t: byte-swapped RGB565, as the panel takes it */
    DRAW_IMAGE_MONO1 =
        DRAW_IMAGE_FORMAT_MONO1, /* one bit, the most significant leftmost, 1 ink; a row is stride / 8 bytes */
} draw_image_format_t;

/* An opened entry. Holds no copy: the pack must outlive it. */
typedef struct {
    union {
        const gfx_color_t* pixels; /* DRAW_IMAGE_RGB565 */
        const uint8_t* bits;       /* DRAW_IMAGE_MONO1 */
    };

    draw_image_format_t format;
    uint16_t width;
    uint16_t height;
    uint32_t stride; /* pixels from one row's start to the next */
} draw_image_t;

/* Checks the entry and returns the first failure: ASSET_ERR_VERSION,
 * ASSET_ERR_FORMAT (a format this does not read, an empty image, a stride shorter
 * than a row or, for MONO1, not whole bytes, rows over the header) or
 * ASSET_ERR_BOUNDS (rows leaving the entry, or RGB565 rows misaligned). Else
 * fills `out`; a caller that draws one format checks `format`. */
asset_status_t draw_image_open(asset_view_t entry, draw_image_t* out);

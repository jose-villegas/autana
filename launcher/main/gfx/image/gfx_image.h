/*
 * gfx_image: a picture as an asset-pack entry (IMAG), written by
 * launcher/tools/gfx/image_asset.py. Opening it checks the header and that
 * every row lies inside the entry; the pixels then point into the entry, so a
 * mapped pack draws with no copy and no RAM. The layout is in
 * docs/assets/README.md.
 *
 * No allocation and no ESP-IDF.
 */
#pragma once

#include <stdint.h>

#include "asset/asset_pack.h"
#include "gfx/draw/gfx_color.h"

#define GFX_IMAGE_ASSET   ASSET_TYPE('I', 'M', 'A', 'G')
#define GFX_IMAGE_VERSION 1U

/* How a pixel is stored. One type holds every picture the engine draws, so a
 * new kind of picture is a new format here, not a new entry type. */
typedef enum {
    GFX_IMAGE_RGB565 = 1, /* gfx_color_t: byte-swapped RGB565, as the panel takes it */
    GFX_IMAGE_MONO1 = 2,  /* one bit, the most significant leftmost, 1 ink; a row is stride / 8 bytes */
} gfx_image_format_t;

/* A MONO1 stride is whole bytes. */
#define GFX_IMAGE_MONO1_STRIDE_STEP 8U

/* An opened entry. Holds no copy: the pack must outlive it. */
typedef struct {
    union {
        const gfx_color_t* pixels; /* GFX_IMAGE_RGB565 */
        const uint8_t* bits;       /* GFX_IMAGE_MONO1 */
    };

    gfx_image_format_t format;
    uint16_t width;
    uint16_t height;
    uint32_t stride; /* pixels from one row's start to the next */
} gfx_image_t;

/* Checks the entry and returns the first failure: ASSET_ERR_VERSION,
 * ASSET_ERR_FORMAT (a format this does not read, an empty image, a stride shorter
 * than a row or, for MONO1, not whole bytes, rows over the header) or
 * ASSET_ERR_BOUNDS (rows leaving the entry, or RGB565 rows misaligned). Else
 * fills `out`; a caller that draws one format checks `format`. */
asset_status_t gfx_image_open(asset_view_t entry, gfx_image_t* out);

/* The IMAG entry `id` of `pack`, opened. */
asset_status_t gfx_image_from_pack(const asset_pack_t* pack, const char* id, gfx_image_t* out);

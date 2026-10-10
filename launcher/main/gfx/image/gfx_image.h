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
    /* 2 to 15 are kept for the icon formats. */
} gfx_image_format_t;

/* An opened entry. Holds no copy: the pack must outlive it. */
typedef struct {
    const gfx_color_t* pixels;
    uint16_t width;
    uint16_t height;
    uint32_t stride; /* pixels from one row's start to the next */
} gfx_image_t;

/* Checks the entry and returns the first failure: ASSET_ERR_VERSION,
 * ASSET_ERR_FORMAT (a format this does not read, an empty image, a stride shorter
 * than a row, rows over the header) or ASSET_ERR_BOUNDS (rows leaving the
 * entry, or misaligned). Else fills `out`. */
asset_status_t gfx_image_open(asset_view_t entry, gfx_image_t* out);

/* The IMAG entry `id` of `pack`, opened. */
asset_status_t gfx_image_from_pack(const asset_pack_t* pack, const char* id, gfx_image_t* out);

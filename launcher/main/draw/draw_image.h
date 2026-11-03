/*
 * draw_image: a picture as an asset-pack entry (IMAG), written by
 * launcher/tools/gfx/image_asset.py. Opening it checks the header and that
 * every row lies inside the entry; the pixels then point into the entry, so a
 * mapped pack draws with no copy and no RAM. A caller finds the entry with
 * asset_pack_find() and DRAW_IMAGE_ASSET. The layout is in
 * docs/assets/formats/IMAG.md.
 *
 * No allocation and no ESP-IDF.
 */
#pragma once

#include <stdint.h>

#include "asset/asset_pack.h"
#include "draw/draw_image_format.h"
#include "image/image.h"

/* Checks the entry and returns the first failure: ASSET_ERR_VERSION,
 * ASSET_ERR_FORMAT (a format this does not read, an empty image, a stride shorter
 * than a row or, for MONO1, not whole bytes, rows over the header) or
 * ASSET_ERR_BOUNDS (rows leaving the entry, or RGB565 rows misaligned). Else
 * fills `out`; a caller that draws one format checks `format`.
 * out->pixels points into the entry: read-only, valid while the pack is mapped. */
asset_status_t draw_image_open(asset_view_t entry, image_t* out);

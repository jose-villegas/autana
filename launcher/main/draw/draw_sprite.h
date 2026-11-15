/*
 * draw_sprite: a picture with coverage and a nine-slice border, drawn
 * stretched into any rectangle. The entry is a SPRT (a border, then an IMAG
 * body) or a bare IMAG (no border); launcher/tools/gfx/image_asset.py writes
 * both and docs/assets/formats/IMAG.md holds the layout.
 *
 * Open a sprite once, when its pack loads, and draw it every frame: opening
 * walks the pack's entries. The pixels point into the pack until
 * draw_sprite_make_resident() copies them into RAM the caller owns.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asset/asset_pack.h"
#include "gfx/target/gfx_color.h"
#include "image/image.h"
#include "math/geometry/recti.h"

/* Pixels at each edge that keep their size when the sprite stretches: the
 * corners are drawn 1:1, the edges stretch along one axis, the centre both. */
typedef struct {
    uint8_t left, top, right, bottom;
} sprite_border_t;

typedef struct {
    image_t color; /* IMAGE_RGB565BE */
    image_t alpha; /* IMAGE_A8, same size; pixels NULL when the sprite is opaque */
    sprite_border_t border;
    /* A centre that is one colour at one alpha, found at import: drawn as one
     * blended rectangle instead of pixel by pixel. */
    bool flat_centre;
    uint8_t centre_alpha;
    gfx_color_t centre_color;
} sprite_t;

/* Finds `name` in `pack` and fills `out`. Errors: those of asset_pack_find()
 * and draw_image_open_alpha(); ASSET_ERR_TYPE for an entry neither SPRT nor
 * IMAG; ASSET_ERR_FORMAT for a one-bit picture, a flat flag other than 0 or 1
 * or reserved bytes in use; ASSET_ERR_VERSION for another SPRT version;
 * ASSET_ERR_BOUNDS for a SPRT shorter than its header or a border that leaves
 * the picture no centre column or row. A failure leaves `out` zeroed. */
asset_status_t draw_sprite_open(const asset_pack_t* pack, const char* name, sprite_t* out);

/* The bytes draw_sprite_make_resident() copies: both planes. */
size_t draw_sprite_bytes(const sprite_t* s);

/* Copies the sprite's planes into `dst` (draw_sprite_bytes() long, 4-byte
 * aligned, owned by the caller and outliving the sprite) and repoints `s` at
 * them, so drawing reads RAM instead of the mapped pack. */
void draw_sprite_make_resident(sprite_t* s, void* dst);

/* Fills `dst`, a panel rectangle, with the sprite turned clockwise by
 * `quarter_turns` and nine-sliced to fit (nearest pixel when stretched; a
 * border wider than `dst` shrinks in proportion). Coverage 0 leaves the
 * panel, 255 replaces it and anything between mixes with rgb565be_mix().
 * Clipped by draw_set_clip(). */
void draw_sprite(const sprite_t* s, recti_t dst, int quarter_turns);

/* draw_sprite() with the border drawn `scale` (1 or more) panel pixels to a
 * sprite pixel, for a picture drawn at an integer zoom. */
void draw_sprite_scaled(const sprite_t* s, recti_t dst, int quarter_turns, int scale);

/* draw_image_format: the IMAG file layout stored in an asset pack, and the
 * SPRT entry: a nine-slice border, then an IMAG body. */
#pragma once

#include "asset/asset_pack.h"

#define DRAW_IMAGE_ASSET                ASSET_TYPE('I', 'M', 'A', 'G')
#define DRAW_IMAGE_VERSION              1U
#define DRAW_IMAGE_FORMAT_RGB565        1U
#define DRAW_IMAGE_FORMAT_MONO1         2U
/* RGB565 rows, then as many A8 rows of the same stride (LVGL's RGB565A8). */
#define DRAW_IMAGE_FORMAT_RGB565A8      3U
#define DRAW_IMAGE_MONO1_STRIDE_STEP    8U
/* Keeps every row of both RGB565A8 planes 4-byte aligned. */
#define DRAW_IMAGE_RGB565A8_STRIDE_STEP 4U
#define DRAW_IMAGE_HEADER_SIZE          16U
#define DRAW_IMAGE_AT_FORMAT            2U
#define DRAW_IMAGE_AT_WIDTH             4U
#define DRAW_IMAGE_AT_HEIGHT            6U
#define DRAW_IMAGE_AT_STRIDE            8U
#define DRAW_IMAGE_AT_PIXELS            12U
#define DRAW_IMAGE_PIXEL_ALIGN          4U

#define DRAW_SPRITE_ASSET               ASSET_TYPE('S', 'P', 'R', 'T')
#define DRAW_SPRITE_VERSION             1U
#define DRAW_SPRITE_HEADER_SIZE         12U /* the IMAG body follows, 4-aligned */
#define DRAW_SPRITE_AT_BORDER           2U  /* left, top, right, bottom: one byte each */
#define DRAW_SPRITE_AT_FLAT             6U  /* 1 when the centre is one colour at one alpha, else 0 */
#define DRAW_SPRITE_AT_CENTRE_ALPHA     7U
#define DRAW_SPRITE_AT_CENTRE_COLOR     8U  /* byte-swapped RGB565, as the pixels */
#define DRAW_SPRITE_AT_RESERVED         10U /* two zero bytes */

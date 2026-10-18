/* draw_image_format: the IMAG file layout stored in an asset pack. */
#pragma once

#include "asset/asset_pack.h"

#define DRAW_IMAGE_ASSET             ASSET_TYPE('I', 'M', 'A', 'G')
#define DRAW_IMAGE_VERSION           1U
#define DRAW_IMAGE_FORMAT_RGB565     1U
#define DRAW_IMAGE_FORMAT_MONO1      2U
#define DRAW_IMAGE_MONO1_STRIDE_STEP 8U
#define DRAW_IMAGE_HEADER_SIZE       16U
#define DRAW_IMAGE_AT_FORMAT         2U
#define DRAW_IMAGE_AT_WIDTH          4U
#define DRAW_IMAGE_AT_HEIGHT         6U
#define DRAW_IMAGE_AT_STRIDE         8U
#define DRAW_IMAGE_AT_PIXELS         12U
#define DRAW_IMAGE_PIXEL_ALIGN       4U

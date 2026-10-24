/* lcol_format: mapped colour sets for shared lit-mesh geometry. */
#pragma once

#include "asset/asset_pack.h"

#define LCOL_ASSET           ASSET_TYPE('L', 'C', 'O', 'L')
#define LCOL_VERSION         1U
#define LCOL_ENCODING_RGB888 0U
#define LCOL_ENCODING_RGB565 1U
#define LCOL_HEADER_SIZE     12U
#define LCOL_AT_VERSION      0U
#define LCOL_AT_ENCODING     2U
#define LCOL_AT_SET_COUNT    4U
#define LCOL_AT_COUNT        8U
#define LCOL_RGB888_BYTES    3U
#define LCOL_RGB565_BYTES    2U
#define LCOL_NAME_SUFFIX     ".colors"

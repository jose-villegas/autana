/* r3d_skin_format: the SKIN file layout stored in an asset pack. */
#pragma once

#include "asset/asset_pack.h"

#define R3D_SKIN_ASSET              ASSET_TYPE('S', 'K', 'I', 'N')
#define R3D_SKIN_VERSION            2U
#define R3D_SKIN_LIGHT_DIRECT       0U
#define R3D_SKIN_LIGHT_RENORMALISED 1U
#define R3D_SKIN_LIGHT_TABLE        2U
#define R3D_SKIN_WEIGHT_SUM         255U
#define R3D_SKIN_NORMAL_SCALE       127
#define R3D_SKIN_WEIGHT_RECIP       (1.0F / (float)R3D_SKIN_WEIGHT_SUM)
#define R3D_SKIN_NORMAL_RECIP       (1.0F / (float)R3D_SKIN_NORMAL_SCALE)

#define R3D_SKIN_HEADER_SIZE        20U
#define R3D_SKIN_AT_LIGHTING_MODE   16U
#define R3D_SKIN_AT_PAD             17U
#define R3D_SKIN_AT_VERSION         0U
#define R3D_SKIN_AT_JOINTS          2U
#define R3D_SKIN_AT_INFLUENCES      3U
#define R3D_SKIN_AT_VERTICES        4U
#define R3D_SKIN_AT_INVERSE         8U
#define R3D_SKIN_AT_RECORDS         12U
#define R3D_SKIN_ALIGNMENT          4U
#define R3D_SKIN_MATRIX_WIDTH       12U
#define R3D_SKIN_INFLUENCES_MIN     2U
#define R3D_SKIN_INFLUENCES_MAX     4U
#define R3D_SKIN_NORMAL_WIDTH       3U
#define R3D_SKIN_RECORD_TAIL        (R3D_SKIN_NORMAL_WIDTH + 1U)

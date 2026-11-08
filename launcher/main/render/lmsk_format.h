/* lmsk_format: a lit mesh's one-bit mask atlas and affine texture coordinates. */
#pragma once

#include "asset/asset_pack.h"

#define LMSK_ASSET            ASSET_TYPE('L', 'M', 'S', 'K')
#define LMSK_VERSION          1U
#define LMSK_MASK_WIDTH       32U
#define LMSK_UV_FRACTION_BITS 4U
#define LMSK_HEADER_SIZE      12U
#define LMSK_AT_VERSION       0U
#define LMSK_AT_MASK_WIDTH    2U
#define LMSK_AT_VERTEX_FIRST  4U
#define LMSK_AT_MASK_ROWS     8U
#define LMSK_NAME_SUFFIX      ".mask"

/* The atlas as a fill reads it: one word of LMSK_MASK_WIDTH bits per row, a
 * pixel kept where its u bit is set in the row its v picks. */
typedef struct {
    const uint32_t* rows;
    int row_count;
} lmsk_atlas_t;

/* lmor_format: where each vertex of a lit mesh with levels lands on the
 * coarser level its cluster gives way to, the sibling entry beside its LMSH
 * (docs/assets/formats/LMOR.md). */
#pragma once

#include "asset/asset_pack.h"

#define LMOR_ASSET            ASSET_TYPE('L', 'M', 'O', 'R')
#define LMOR_VERSION          1U
#define LMOR_HEADER_SIZE      12U
#define LMOR_AT_VERSION       0U
#define LMOR_AT_FLAGS         2U
#define LMOR_AT_CLUSTER_COUNT 4U
#define LMOR_AT_VERTEX_COUNT  8U
/* flags: the colour targets follow the deltas. */
#define LMOR_COLOURS          1U
/* A delta's three int8 axes, and a colour target's three channels. */
#define LMOR_AXES             3U
/* The largest shift: a delta's int8 << it spans int16's range. */
#define LMOR_SHIFT_MAX        16U
#define LMOR_NAME_SUFFIX      ".morph"

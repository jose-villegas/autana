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
/* flags: each vertex's landing point on the coarse level follows the deltas,
 * so its colour target comes from whichever colours the mesh draws with. */
#define LMOR_LANDING          1U
/* A delta's three int8 axes. */
#define LMOR_AXES             3U
/* A landing point: a uint16 coarse triangle and two uint8 corner weights,
 * the third corner's LMOR_WEIGHT_WHOLE minus both. */
#define LMOR_LANDING_SIZE     4U
#define LMOR_WEIGHT_WHOLE     255U
/* The largest shift: a delta's int8 << it spans int16's range. */
#define LMOR_SHIFT_MAX        16U
#define LMOR_NAME_SUFFIX      ".morph"

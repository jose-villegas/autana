/* llod_format: a lit mesh's levels of detail, the sibling entry beside its LMSH. */
#pragma once

#include "asset/asset_pack.h"

#define LLOD_ASSET            ASSET_TYPE('L', 'L', 'O', 'D')
#define LLOD_VERSION          1U
#define LLOD_HEADER_SIZE      8U
#define LLOD_AT_VERSION       0U
#define LLOD_AT_GROUP_COUNT   2U
#define LLOD_AT_CLUSTER_COUNT 4U
#define LLOD_NAME_SUFFIX      ".lod"

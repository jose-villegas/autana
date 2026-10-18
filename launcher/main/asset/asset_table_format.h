/* asset_table_format: the TABL file layout stored in an asset pack. */
#pragma once

#include "asset/asset_pack.h"

#define ASSET_TABLE_ASSET        ASSET_TYPE('T', 'A', 'B', 'L')
#define ASSET_TABLE_VERSION      1U
#define ASSET_TABLE_HEADER_SIZE  8U
#define ASSET_TABLE_NAME_SIZE    24U
#define ASSET_TABLE_AT_ROWS      2U
#define ASSET_TABLE_AT_COLUMNS   4U
#define ASSET_TABLE_AT_FRAC_BITS 6U
#define ASSET_TABLE_AT_RESERVED  7U

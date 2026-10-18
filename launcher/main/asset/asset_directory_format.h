/* asset_directory_format: the ABDR file layout of a region containing asset packs. */
#pragma once

#define ASSET_DIRECTORY_MAGIC          "ABDR"
#define ASSET_DIRECTORY_VERSION        2U
#define ASSET_DIRECTORY_HEADER_SIZE    16U
#define ASSET_DIRECTORY_ROW_SIZE       40U
#define ASSET_MMU_PAGE_SIZE            65536U /* Separate pages keep mapping lifetimes independent. */

#define ASSET_DIRECTORY_HEADER_CRC     4U
#define ASSET_DIRECTORY_HEADER_VERSION 8U
#define ASSET_DIRECTORY_HEADER_COUNT   12U
#define ASSET_DIRECTORY_ROW_OFFSET     32U
#define ASSET_DIRECTORY_ROW_AT_SIZE    36U

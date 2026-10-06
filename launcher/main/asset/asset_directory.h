/*
 * asset_directory: where each pack lies in a region that holds several, as
 * the device's assets partition does. The directory is a header ("ABDR", a
 * CRC-32 of the rest of it, version, count) and a row per pack: name, offset
 * from the region's start, size. Each
 * pack starts on its own sector, so it maps alone and can be rewritten
 * alone. The layout is in docs/assets/README.md and is written by
 * launcher/tools/asset/asset_pack.py.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "asset/asset_pack.h"

#define ASSET_DIRECTORY_MAGIC       "ABDR"
#define ASSET_DIRECTORY_VERSION     1U
#define ASSET_DIRECTORY_HEADER_SIZE 16U
#define ASSET_DIRECTORY_ROW_SIZE    40U
#define ASSET_PACK_ALIGN            4096U /* a flash sector */

/* A validated directory. Holds no copy: `base` must outlive it. */
typedef struct {
    const uint8_t* base;
    uint32_t count;
} asset_directory_t;

/* Where one pack lies, from the region's start. */
typedef struct {
    uint32_t offset;
    uint32_t size;
} asset_slot_t;

/* The size of the directory (header and rows) from at least
 * ASSET_DIRECTORY_HEADER_SIZE bytes at `head`; 0 when they are not a
 * directory's. How much to read before opening it. */
uint32_t asset_directory_size(const void* head, size_t available);

/* Checks the header, the checksum and every row: inside `region` bytes, on a
 * pack sector, after the rows and the pack before it (ASSET_ERR_BOUNDS),
 * and no name twice (ASSET_ERR_DUPLICATE); then fills `directory`. `size` is
 * what is readable at `base`. */
asset_status_t asset_directory_open(asset_directory_t* directory, const void* base, size_t size, uint32_t region);

/* The slot of pack `name`; ASSET_ERR_NOT_FOUND when no row has it. */
asset_status_t asset_directory_find(const asset_directory_t* directory, const char* name, asset_slot_t* slot);

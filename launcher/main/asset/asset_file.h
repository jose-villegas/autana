/*
 * asset_file: an asset pack read from a file, for a host build. The same
 * asset_pack_open() checks it that checks a mapped partition.
 */
#pragma once

#include "asset/asset_pack.h"

/* Reads and validates the pack at `path`. On success `*buffer` owns the
 * bytes `pack` points into and the caller releases it with heap_caps_free();
 * on failure it is NULL. A file that cannot be read is ASSET_ERR_NO_PACK. */
asset_status_t asset_file_open(const char* path, asset_pack_t* pack, void** buffer);

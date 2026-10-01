/*
 * asset_file: an asset pack read from a file, for a host build. The same
 * asset_pack_open() checks it that checks a mapped partition. The bytes are
 * held outside the host tests' modelled heap, so a pack costs a test's
 * budget nothing, as a mapped partition costs the board none.
 */
#pragma once

#include "asset/asset_pack.h"

/* Reads and validates the pack at `path`. On success `*buffer` owns the
 * bytes `pack` points into and the caller releases it with
 * asset_file_release(); on failure it is NULL. A file that cannot be read is
 * ASSET_ERR_NO_PACK. */
asset_status_t asset_file_open(const char* path, asset_pack_t* pack, void** buffer);

void asset_file_release(void* buffer);

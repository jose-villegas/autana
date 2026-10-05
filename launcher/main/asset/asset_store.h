/*
 * asset_store: the asset bundles this build reads, each an asset pack named
 * after its root asset. A bundle is mounted and checked alone on first use
 * and counted, so checking one costs its own size only. A device maps it from
 * the "assets" partition, where a bundle directory says where it lies; a host
 * reads <dir>/<name>.apak, dir being AUTANA_ASSET_DIR, else the folder a
 * renderer was built with (ASSET_DIR_DEFAULT_PATH). Either way the bytes go
 * to asset_pack_open(), which does not care where they came from. Call it
 * from one task.
 */
#pragma once

#include "asset/asset_pack.h"

/* How many bundles may be mounted at once. */
#define ASSET_STORE_MOUNTS_MAX 8

/* Bundle `name`, mounted and checked on its first use and counted after. NULL
 * when it is missing or bad, with one log line saying why; a caller leaves
 * its content out rather than draw from nothing. */
const asset_pack_t* asset_store_bundle(const char* name);

/* Drops one use of bundle `name`; at none it is unmapped or freed, and the
 * pack asset_store_bundle() returned for it is gone. */
void asset_store_release(const char* name);

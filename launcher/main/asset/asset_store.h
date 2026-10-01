/*
 * asset_store: the one asset pack this build reads. A device maps the
 * "assets" flash partition and uses it in place; a host reads the file named
 * by AUTANA_ASSET_PACK, else launcher/assets/assets.bin beside the sources.
 * Either way the bytes go to asset_pack_open(), which does not care where
 * they came from.
 */
#pragma once

#include "asset/asset_pack.h"

/* The pack, opened and checked on the first call and kept for good. NULL when
 * it cannot be: the reason is logged, and a caller leaves its content out
 * rather than draw from nothing. Call it from one task. */
const asset_pack_t* asset_store_pack(void);

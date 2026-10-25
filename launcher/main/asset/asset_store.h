/* asset_store: counted pack mounts from the assets partition or a host folder
 * (AUTANA_ASSET_DIR, else ASSET_DIR_DEFAULT_PATH), validated incrementally on core 0. */
#pragma once

#include "asset/asset_pack.h"
#include "core/pending.h"

#define ASSET_ENGINE_PACK      "engine"

/* The variable that names a host's pack folder. */
#define ASSET_STORE_DIR_ENV    "AUTANA_ASSET_DIR"

/* How many packs may be mounted at once. */
#define ASSET_STORE_MOUNTS_MAX 8

/* Pack `name`, mounted and checked on its first use and counted after. NULL
 * when it is missing or bad, with one log line saying why; a caller leaves
 * its content out rather than draw from nothing. */
const asset_pack_t* asset_store_load(const char* name);

#define ASSET_STORE_REQUESTS_MAX 16U
/* Bounds CRC work per pass to a small flash-cache read; entry checks share this budget. */
#define ASSET_STORE_STEP_BYTES   4096U

/* Completion runs from the frame loop on core 0, never inside this call.
 * State     | Lifetime
 * WAIT      | Until complete or cancelled
 * READY     | Until release(name), invalidating the oldest READY request of that name
 * FAILED    | Until pool pressure reclaims it, oldest first
 * CANCELLED | Immediately reusable
 * NONE      | Zero, stale or released handle
 * HANDLE_NONE owns the current visit. */
handle_t asset_store_load_async(const char* name, pending_fn done, handle_t owner);
pending_t asset_store_state(handle_t load);
const asset_pack_t* asset_store_loaded(handle_t load);
/* Before READY: frees partial work and suppresses completion immediately. */
void asset_store_cancel(handle_t load);

/* How many packs are mounted now: a pack kept for the whole run, such as the
 * engine's artwork, holds its slot for good. */
int asset_store_mounted(void);

/* Drops one use of pack `name`; at none it is unmapped or freed, and the
 * pack asset_store_load() returned for it is gone. */
void asset_store_release(const char* name);

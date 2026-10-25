/* asset_store: counted pack mounts from the assets partition or a host folder
 * (AUTANA_ASSET_DIR, else ASSET_DIR_DEFAULT_PATH), validated incrementally. Core 0 only. */
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
 * its content out rather than draw from nothing. Blocks until validated. */
const asset_pack_t* asset_store_load(const char* name);

#define ASSET_STORE_REQUESTS_MAX 16U
/* CRC and entry-table bytes per pass, shared by every pending mount; placeholder until measured on the board. */
#define ASSET_STORE_STEP_BYTES   4096U

/* Completion runs from the frame loop on core 0, never inside this call.
 * Lifetimes: docs/assets/Loading-Packs.md#async-loading.
 * HANDLE_NONE owns requests the loop cancels when the app exits. */
handle_t asset_store_load_async(const char* name, pending_fn done, handle_t owner);
pending_t asset_store_state(handle_t load);
const asset_pack_t* asset_store_loaded(handle_t load);
/* Suppresses completion; before READY also frees partial work. READY keeps its pack use. */
void asset_store_cancel(handle_t load);

/* How many packs are mounted now: a pack kept for the whole run, such as the
 * engine's artwork, holds its slot for good. */
int asset_store_mounted(void);

/* Drops one use of pack `name`; at none it is unmapped or freed, and the
 * pack asset_store_load() returned for it is gone. */
void asset_store_release(const char* name);

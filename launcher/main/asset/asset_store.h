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

/* Returns READY or FAILED after inline validation, or HANDLE_NONE when full.
 * A failure logs one reason; the caller leaves its content out. Release every handle. */
handle_t asset_store_load(const char* name);

/* Handles beyond one per mounted pack: second holders of a pack (the engine pack's ridge and
 * icon sets) and async requests still waiting for a mount. */
#define ASSET_STORE_EXTRA_HANDLES 4U
/* Live handles, blocking and async, at once. When full, a load logs and returns HANDLE_NONE;
 * it never drops a holder. */
#define ASSET_STORE_REQUESTS_MAX  (ASSET_STORE_MOUNTS_MAX + ASSET_STORE_EXTRA_HANDLES)
/* CRC and entry-table bytes per pass, shared by every pending mount; placeholder until measured on the board. */
#define ASSET_STORE_STEP_BYTES    4096U

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

/* Retires this handle, cancelling WAIT or dropping its READY use. Stale handles are harmless. */
void asset_store_release(handle_t load);

/*
 * asset_store_backend: where a bundle's bytes come from, for the counted
 * mounts in asset_store.c. One backend is linked: asset_store_flash.c on the
 * device, asset_store_file.c on a host. Not for callers of the store.
 */
#pragma once

#include <stdint.h>

#include "asset/asset_pack.h"

/* Maps or reads bundle `name` and opens it into `pack`. On success `mapping`
 * is what asset_store_backend_unmount() takes back; on failure nothing is
 * held. Logs nothing. */
asset_status_t asset_store_backend_mount(const char* name, asset_pack_t* pack, uintptr_t* mapping);

void asset_store_backend_unmount(uintptr_t mapping);

/* The one log line for a mount of `name` that failed with `status`. */
void asset_store_backend_report(const char* name, asset_status_t status);

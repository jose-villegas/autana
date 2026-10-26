/*
 * The store under suite_reads_probe.c: every pack absent. It is its own
 * object because --wrap=asset_store_load reaches only calls from another one.
 */
#include "asset/asset_store.h"

handle_t
asset_store_load(const char* name) {
    return HANDLE_NONE;
}

void
asset_store_release(handle_t load) {}

/*
 * The store under suite_reads_probe.c: every pack absent. It is its own
 * object because --wrap=asset_store_pack reaches only calls from another one.
 */
#include "asset/asset_store.h"

const asset_pack_t*
asset_store_pack(const char* name) {
    return NULL;
}

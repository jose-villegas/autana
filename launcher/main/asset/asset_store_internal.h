/* asset_store_internal: shared mount requests for inline and frame-loop loading. */
#pragma once
#include "asset/asset_store.h"

typedef struct asset_store_mount asset_store_mount_t;

typedef struct {
    char name[ASSET_NAME_MAX];
    handle_t owner;
    pending_fn done;
    asset_store_mount_t* mount;
    uint64_t order;
    uint16_t generation;
    uint8_t state;
    bool cancelled;
    bool async;
} asset_store_request_t;

asset_store_request_t* asset_store_request_at(uint32_t slot);
asset_store_request_t* asset_store_request_find(handle_t load);
handle_t asset_store_request_id(const asset_store_request_t* request);
handle_t asset_store_request_new(const char* name);
void asset_store_request_bind(asset_store_request_t* request, bool blocking);
void asset_store_request_finish(asset_store_request_t* request);
void asset_store_request_cancel(asset_store_request_t* request);
void asset_store_mounts_step(size_t budget);

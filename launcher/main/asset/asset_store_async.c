/* asset_store_async: owner checks and completion delivery on core 0. */
#include <stddef.h>
#include "asset/asset_store_internal.h"
#include "asset/asset_store_pending.h"

handle_t
asset_store_load_async(const char* name, pending_fn done, handle_t owner) {
    const handle_t load = asset_store_request_new(name);
    asset_store_request_t* request = asset_store_request_find(load);
    if (request != NULL) {
        request->owner = owner;
        request->done = done;
        if (!pending_owner_alive(owner)) {
            asset_store_request_cancel(request);
        }
    }
    return load;
}

void
asset_store_cancel(handle_t load) {
    asset_store_request_t* request = asset_store_request_find(load);
    if (request != NULL) {
        if (request->state == PENDING_READY) {
            request->done = NULL;
        } else {
            asset_store_request_cancel(request);
        }
    }
}

static bool
check_owner(asset_store_request_t* request) {
    if (!pending_owner_alive(request->owner)) {
        asset_store_request_cancel(request);
        return false;
    }
    return true;
}

void
asset_store_step(void) {
    uint64_t cutoff = 0;
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        const asset_store_request_t* request = asset_store_request_at(i);
        if (request->order > cutoff) {
            cutoff = request->order;
        }
    }
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_request_t* request = asset_store_request_at(i);
        if ((request->state == PENDING_WAIT || request->state == PENDING_READY || request->state == PENDING_FAILED)
            && check_owner(request) && request->state == PENDING_WAIT && request->mount == NULL) {
            asset_store_request_bind(request);
        }
    }
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_request_t* request = asset_store_request_at(i);
        if (request->state != PENDING_WAIT || request->mount == NULL) {
            continue;
        }
        bool stepped = false;
        for (uint32_t j = 0; j < i; j++) {
            if (asset_store_request_at(j)->state == PENDING_WAIT
                && asset_store_request_at(j)->mount == request->mount) {
                stepped = true;
                break;
            }
        }
        if (!stepped) {
            asset_store_mount_step(request->mount);
        }
    }
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_request_t* request = asset_store_request_at(i);
        if (request->order > cutoff) {
            continue;
        }
        if (request->state == PENDING_WAIT) {
            asset_store_request_finish(request);
        }
        if ((request->state == PENDING_READY || request->state == PENDING_FAILED) && request->done != NULL
            && check_owner(request)) {
            pending_fn done = request->done;
            request->done = NULL;
            done(asset_store_request_id(request), (pending_t)request->state, request->owner);
        }
    }
}

void
asset_store_app_exit(void) {
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_request_t* request = asset_store_request_at(i);
        if (handle_is_none(request->owner)
            && (request->state == PENDING_WAIT || request->state == PENDING_READY
                || request->state == PENDING_FAILED)) {
            asset_store_request_cancel(request);
        }
    }
}

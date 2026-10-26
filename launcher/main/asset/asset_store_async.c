/* asset_store_async: owner checks and completion delivery on core 0. */
#include <stddef.h>
#include "asset/asset_store_internal.h"
#include "asset/asset_store_shell.h"

handle_t
asset_store_load_async(const char* name, pending_fn done, handle_t owner) {
    const handle_t load = asset_store_request_new(name);
    asset_store_request_t* request = asset_store_request_find(load);
    if (request != NULL) {
        request->async = true;
        request->owner = owner;
        request->done = done;
        if (!pending_owner_alive(owner)) {
            asset_store_cancel(load);
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
            request->cancelled = true;
        } else {
            asset_store_request_cancel(request);
        }
    }
}

static bool
check_owner(asset_store_request_t* request) {
    if (!request->async) {
        return true;
    }
    if (request->cancelled) {
        return false;
    }
    if (!pending_owner_alive(request->owner)) {
        asset_store_cancel(asset_store_request_id(request));
        return false;
    }
    return true;
}

static bool
is_live(const asset_store_request_t* request) {
    return request->state == PENDING_WAIT || request->state == PENDING_READY || request->state == PENDING_FAILED;
}

/* Requests made after this order, inside a callback, wait for the next pass. */
static uint64_t
newest_order(void) {
    uint64_t newest = 0;
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        const uint64_t order = asset_store_request_at(i)->order;
        newest = order > newest ? order : newest;
    }
    return newest;
}

static void
bind_waiting(void) {
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_request_t* request = asset_store_request_at(i);
        if (is_live(request) && check_owner(request) && request->state == PENDING_WAIT && request->mount == NULL) {
            asset_store_request_bind(request, false);
        }
    }
}

static void
deliver(asset_store_request_t* request) {
    if (request->state == PENDING_WAIT) {
        asset_store_request_finish(request);
    }
    if ((request->state == PENDING_READY || request->state == PENDING_FAILED) && request->done != NULL) {
        pending_fn done = request->done;
        request->done = NULL;
        done(asset_store_request_id(request), (pending_t)request->state, request->owner);
    }
}

void
asset_store_step(uint32_t dt_ms) {
    (void)dt_ms;
    const uint64_t cutoff = newest_order();
    bind_waiting();
    asset_store_mounts_step(ASSET_STORE_STEP_BYTES);
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_request_t* request = asset_store_request_at(i);
        if (request->order <= cutoff && is_live(request) && check_owner(request)) {
            deliver(request);
        }
    }
}

void
asset_store_app_exit(void) {
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_request_t* request = asset_store_request_at(i);
        if (request->async && is_live(request)) {
            asset_store_request_cancel(request);
        }
    }
}

#include "asset/asset_store.h"

#include <assert.h>
#include <string.h>

#include "asset/asset_store_backend.h"
#include "asset/asset_store_internal.h"

static asset_store_mount_t mounts[ASSET_STORE_MOUNTS_MAX];
static asset_store_request_t requests[ASSET_STORE_REQUESTS_MAX];
static uint64_t next_order;

static asset_store_mount_t*
find(const char* name) {
    for (int i = 0; i < ASSET_STORE_MOUNTS_MAX; i++) {
        if (mounts[i].name[0] != '\0' && strcmp(mounts[i].name, name) == 0) {
            return &mounts[i];
        }
    }
    return NULL;
}

static void
unmount_unused(asset_store_mount_t* mount) {
    if (mount->uses == 0 && mount->waiting == 0) {
        asset_store_backend_unmount(mount->mapping);
        *mount = (asset_store_mount_t){0};
    }
}

asset_store_request_t*
asset_store_request_find(handle_t load) {
    const uint32_t slot = handle_slot(load);
    if (handle_owner(load) != HANDLE_OWNER_ASSET_LOAD || slot >= ASSET_STORE_REQUESTS_MAX
        || handle_generation(load) == 0 || requests[slot].generation != handle_generation(load)
        || requests[slot].state == PENDING_NONE) {
        return NULL;
    }
    return &requests[slot];
}

handle_t
asset_store_request_id(const asset_store_request_t* request) {
    return handle_make(HANDLE_OWNER_ASSET_LOAD, (uint32_t)(request - requests), request->generation);
}

static asset_store_request_t*
allocate_request(void) {
    asset_store_request_t* oldest = NULL;
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_request_t* request = &requests[i];
        if (!handle_generation_next(request->generation)) {
            continue;
        }
        if (request->state == PENDING_NONE || request->state == PENDING_CANCELLED) {
            return request;
        }
        if (request->state == PENDING_FAILED && request->done == NULL
            && (oldest == NULL || request->order < oldest->order)) {
            oldest = request;
        }
    }
    return oldest;
}

static void
publish(asset_store_request_t* request, asset_store_mount_t* mount) {
    request->mount = mount;
    mount->uses++;
    /* Core 0 owns every access, so no atomics are needed; publish the result before the state for a worker. */
    request->state = PENDING_READY;
}

handle_t
asset_store_request_new(const char* name) {
    asset_store_request_t* request = allocate_request();
    if (request == NULL) {
        asset_store_backend_report(name, ASSET_ERR_FULL);
        return HANDLE_NONE;
    }
    const uint16_t generation = request->generation + 1U;
    *request = (asset_store_request_t){.generation = generation, .state = PENDING_WAIT, .order = ++next_order};
    if (name == NULL || name[0] == '\0' || strlen(name) >= ASSET_NAME_MAX) {
        asset_store_backend_report(name == NULL ? "(null)" : name, ASSET_ERR_NOT_FOUND);
        request->state = PENDING_FAILED;
        return asset_store_request_id(request);
    }
    (void)strcpy(request->name, name);
    asset_store_mount_t* mount = find(name);
    if (mount != NULL && mount->opened && mount->status == ASSET_OK) {
        publish(request, mount);
    }
    return asset_store_request_id(request);
}

static void
fail(asset_store_request_t* request, asset_status_t status) {
    asset_store_backend_report(request->name, status);
    request->mount = NULL;
    request->state = PENDING_FAILED;
}

void
asset_store_request_bind(asset_store_request_t* request) {
    asset_store_mount_t* mount = find(request->name);
    if (mount == NULL) {
        for (int i = 0; i < ASSET_STORE_MOUNTS_MAX; i++) {
            if (mounts[i].name[0] == '\0') {
                mount = &mounts[i];
                break;
            }
        }
        if (mount == NULL) {
            fail(request, ASSET_ERR_FULL);
            return;
        }
        const asset_status_t status = asset_store_backend_mount(request->name, &mount->pack, &mount->mapping);
        if (status != ASSET_OK) {
            fail(request, status);
            return;
        }
        (void)strcpy(mount->name, request->name);
        mount->status = asset_pack_open_begin(&mount->opener, mount->pack.base, mount->pack.size);
        mount->opened = mount->status != ASSET_OK;
        mount->pack = (asset_pack_t){0};
    }
    request->mount = mount;
    mount->waiting++;
}

void
asset_store_mount_step(asset_store_mount_t* mount) {
    if (!mount->opened && asset_pack_open_step(&mount->opener, ASSET_STORE_STEP_BYTES)) {
        mount->status = mount->opener.status;
        if (mount->status == ASSET_OK) {
            mount->pack = mount->opener.pack;
        }
        mount->opened = true;
    }
}

void
asset_store_request_finish(asset_store_request_t* request) {
    asset_store_mount_t* mount = request->mount;
    if (mount == NULL || !mount->opened) {
        return;
    }
    mount->waiting--;
    if (mount->status == ASSET_OK) {
        publish(request, mount);
    } else {
        fail(request, mount->status);
        unmount_unused(mount);
    }
}

pending_t
asset_store_state(handle_t load) {
    const asset_store_request_t* request = asset_store_request_find(load);
    return request == NULL ? PENDING_NONE : (pending_t)request->state;
}

const asset_pack_t*
asset_store_loaded(handle_t load) {
    const asset_store_request_t* request = asset_store_request_find(load);
    return request != NULL && request->state == PENDING_READY ? &request->mount->pack : NULL;
}

void
asset_store_request_cancel(asset_store_request_t* request) {
    asset_store_mount_t* mount = request->mount;
    if (mount != NULL) {
        if (request->state == PENDING_WAIT) {
            mount->waiting--;
        } else if (request->state == PENDING_READY) {
            mount->uses--;
        }
        unmount_unused(mount);
    }
    request->done = NULL;
    request->mount = NULL;
    request->state = PENDING_CANCELLED;
}

const asset_pack_t*
asset_store_load(const char* name) {
    const handle_t load = asset_store_request_new(name);
    asset_store_request_t* request = asset_store_request_find(load);
    if (request == NULL) {
        return NULL;
    }
    while (request->state == PENDING_WAIT) {
        if (request->mount == NULL) {
            asset_store_request_bind(request);
        }
        if (request->state == PENDING_WAIT) {
            asset_store_mount_step(request->mount);
            asset_store_request_finish(request);
        }
    }
    const asset_pack_t* pack = asset_store_loaded(load);
    request->state = PENDING_NONE;
    return pack;
}

int
asset_store_mounted(void) {
    int count = 0;
    for (int i = 0; i < ASSET_STORE_MOUNTS_MAX; i++) {
        count += mounts[i].uses > 0 ? 1 : 0;
    }
    return count;
}

void
asset_store_release(const char* name) {
    asset_store_mount_t* mount = find(name);
    assert(mount != NULL && mount->uses > 0);
    if (mount == NULL || mount->uses == 0) {
        return;
    }
    asset_store_request_t* oldest = NULL;
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_request_t* request = &requests[i];
        if (request->state == PENDING_READY && request->mount == mount
            && (oldest == NULL || request->order < oldest->order)) {
            oldest = request;
        }
    }
    if (oldest != NULL) {
        oldest->done = NULL;
        oldest->mount = NULL;
        oldest->state = PENDING_NONE;
    }
    mount->uses--;
    unmount_unused(mount);
}

asset_store_request_t*
asset_store_request_at(uint32_t slot) {
    return slot < ASSET_STORE_REQUESTS_MAX ? &requests[slot] : NULL;
}

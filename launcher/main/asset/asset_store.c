#include "asset/asset_store.h"

#include <assert.h>
#include <string.h>

#include "asset/asset_store_backend.h"
#include "asset/asset_store_internal.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if CONFIG_LAUNCHER_DEVELOPMENT
#include "core/timing.h"
#include "esp_log.h"
#endif

struct asset_store_mount {
    char name[ASSET_NAME_MAX];
    uint32_t uses, waiting;
    asset_pack_t pack;
    uintptr_t mapping;
    asset_pack_opener_t opener;
    asset_status_t status;
    bool opened;
#if CONFIG_LAUNCHER_DEVELOPMENT
    bool async;
    uint32_t passes;
    int64_t validation_us;
#endif
};

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

static void
initialize_request(asset_store_request_t* request, const char* name) {
    if (name == NULL || name[0] == '\0' || strlen(name) >= ASSET_NAME_MAX) {
        asset_store_backend_report(name == NULL ? "(null)" : name, ASSET_ERR_NOT_FOUND);
        request->state = PENDING_FAILED;
        return;
    }
    (void)strcpy(request->name, name);
    asset_store_mount_t* mount = find(name);
    if (mount != NULL && mount->opened && mount->status == ASSET_OK) {
        publish(request, mount);
    }
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
    initialize_request(request, name);
    return asset_store_request_id(request);
}

static void
fail(asset_store_request_t* request, asset_status_t status) {
    asset_store_backend_report(request->name, status);
    request->mount = NULL;
    request->state = PENDING_FAILED;
}

static void
complete_mount(asset_store_mount_t* mount) {
    if (mount->status == ASSET_OK) {
        mount->pack = mount->opener.pack;
#if CONFIG_LAUNCHER_DEVELOPMENT
        ESP_LOGI("asset", "pack %s: %u entries, %u bytes", mount->name, (unsigned)mount->pack.count,
                 (unsigned)mount->pack.size);
#endif
    } else {
        asset_store_backend_report(mount->name, mount->status);
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    if (mount->async) {
        const uint32_t bytes =
            mount->opener.offset >= ASSET_PACK_HEADER_SIZE ? mount->opener.offset - ASSET_PACK_HEADER_SIZE : 0;
        ESP_LOGI("asset", "pack %s validation: %u bytes, %u passes, %lld us", mount->name, (unsigned)bytes,
                 (unsigned)mount->passes, (long long)mount->validation_us);
    }
#endif
    mount->opened = true;
}

void
asset_store_request_bind(asset_store_request_t* request, bool blocking) {
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
#if CONFIG_LAUNCHER_DEVELOPMENT
        mount->async = !blocking;
        const int64_t started = timing_now_us();
#endif
        mount->status = asset_pack_open_begin(&mount->opener, mount->pack.base, mount->pack.size);
#if CONFIG_LAUNCHER_DEVELOPMENT
        mount->validation_us += timing_now_us() - started;
#endif
        mount->opened = mount->status != ASSET_OK;
        mount->pack = (asset_pack_t){0};
        if (mount->opened) {
            complete_mount(mount);
        }
    }
    request->mount = mount;
    mount->waiting++;
}

static size_t
mount_step(asset_store_mount_t* mount, size_t budget) {
    if (mount->opened) {
        return 0;
    }
    const uint32_t before = mount->opener.offset;
    const uint32_t entry_before = mount->opener.entry;
#if CONFIG_LAUNCHER_DEVELOPMENT
    const int64_t started = timing_now_us();
    mount->passes++;
#endif
    const bool complete = asset_pack_open_step(&mount->opener, budget);
#if CONFIG_LAUNCHER_DEVELOPMENT
    mount->validation_us += timing_now_us() - started;
#endif
    if (complete) {
        mount->status = mount->opener.status;
        complete_mount(mount);
    }
    return (mount->opener.offset - before) + (size_t)(mount->opener.entry - entry_before) * ASSET_PACK_ENTRY_SIZE;
}

void
asset_store_mounts_step(size_t budget) {
    for (uint32_t i = 0; i < ASSET_STORE_MOUNTS_MAX && budget > 0; i++) {
        asset_store_mount_t* mount = &mounts[i];
        if (mount->name[0] == '\0' || mount->opened) {
            continue;
        }
        budget -= mount_step(mount, budget);
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
        request->mount = NULL;
        request->state = PENDING_FAILED;
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
    asset_store_request_t local = {.state = PENDING_WAIT};
    asset_store_request_t* request = &local;
    initialize_request(request, name);
    while (request->state == PENDING_WAIT) {
        if (request->mount == NULL) {
            asset_store_request_bind(request, true);
        }
        if (request->state == PENDING_WAIT) {
            (void)mount_step(request->mount, SIZE_MAX);
            asset_store_request_finish(request);
        }
    }
    return request->state == PENDING_READY ? &request->mount->pack : NULL;
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
    uint32_t ready = 0;
    for (uint32_t i = 0; i < ASSET_STORE_REQUESTS_MAX; i++) {
        asset_store_request_t* request = &requests[i];
        if (request->state == PENDING_READY && request->mount == mount) {
            ready++;
            if (oldest == NULL || request->order < oldest->order) {
                oldest = request;
            }
        }
    }
    if (oldest != NULL && mount->uses == ready) {
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

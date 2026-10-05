#include "asset/asset_store.h"

#include <assert.h>
#include <string.h>

#include "asset/asset_store_backend.h"

typedef struct {
    char name[ASSET_NAME_MAX];
    uint32_t uses; /* 0: the slot is free */
    asset_pack_t pack;
    uintptr_t mapping;
} mount_t;

static mount_t mounts[ASSET_STORE_MOUNTS_MAX];

static mount_t*
find(const char* name) {
    for (int i = 0; i < ASSET_STORE_MOUNTS_MAX; i++) {
        if (mounts[i].uses > 0 && strcmp(mounts[i].name, name) == 0) {
            return &mounts[i];
        }
    }
    return NULL;
}

static mount_t*
free_slot(void) {
    for (int i = 0; i < ASSET_STORE_MOUNTS_MAX; i++) {
        if (mounts[i].uses == 0) {
            return &mounts[i];
        }
    }
    return NULL;
}

static asset_status_t
mount(const char* name, mount_t** out) {
    *out = find(name);
    if (*out != NULL) {
        (*out)->uses++;
        return ASSET_OK;
    }
    const size_t length = strlen(name);
    if (length >= ASSET_NAME_MAX) {
        return ASSET_ERR_NOT_FOUND;
    }
    mount_t* slot = free_slot();
    if (slot == NULL) {
        return ASSET_ERR_FULL;
    }
    const asset_status_t status = asset_store_backend_mount(name, &slot->pack, &slot->mapping);
    if (status == ASSET_OK) {
        (void)memcpy(slot->name, name, length + 1U);
        slot->uses = 1;
        *out = slot;
    }
    return status;
}

const asset_pack_t*
asset_store_bundle(const char* name) {
    mount_t* mounted;
    const asset_status_t status = mount(name, &mounted);
    if (status != ASSET_OK) {
        asset_store_backend_report(name, status);
        return NULL;
    }
    return &mounted->pack;
}

void
asset_store_release(const char* name) {
    mount_t* mounted = find(name);
    assert(mounted != NULL);
    if (mounted == NULL || --mounted->uses > 0) {
        return;
    }
    asset_store_backend_unmount(mounted->mapping);
    *mounted = (mount_t){0};
}

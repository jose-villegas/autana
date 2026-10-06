/*
 * asset_store_file: the asset store's host backend, reading one file per
 * bundle from AUTANA_ASSET_DIR.
 */
#include <stdio.h>
#include <stdlib.h>

#include "asset/asset_file.h"
#include "asset/asset_store_backend.h"

#define PATH_BYTES 1024

static const char*
bundle_dir(void) {
    const char* dir = getenv("AUTANA_ASSET_DIR");
#ifdef ASSET_DIR_DEFAULT_PATH
    dir = dir != NULL ? dir : ASSET_DIR_DEFAULT_PATH;
#endif
    return dir;
}

asset_status_t
asset_store_backend_mount(const char* name, asset_pack_t* pack, uintptr_t* mapping) {
    const char* dir = bundle_dir();
    char path[PATH_BYTES];
    if (dir == NULL || snprintf(path, sizeof path, "%s/%s.apak", dir, name) >= (int)sizeof path) {
        return ASSET_ERR_NO_PACK;
    }
    void* buffer = NULL;
    const asset_status_t status = asset_file_open(path, pack, &buffer);
    *mapping = (uintptr_t)buffer;
    return status;
}

void
asset_store_backend_unmount(uintptr_t mapping) {
    asset_file_release((void*)mapping);
}

void
asset_store_backend_report(const char* name, asset_status_t status) {
    const char* dir = bundle_dir();
    (void)fprintf(stderr, "asset bundle %s/%s.apak: %s (AUTANA_ASSET_DIR names the folder build_pack.py wrote)\n",
                  dir == NULL ? "(unset)" : dir, name, asset_status_text(status));
}

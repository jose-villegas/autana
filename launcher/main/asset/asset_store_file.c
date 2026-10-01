#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_file.h"
#include "asset/asset_store.h"

/* Where the sources keep the committed pack, found from this file's own path. */
static const char*
source_pack_path(char* out, size_t capacity) {
    const char* here = __FILE__;
    const char* slash = strrchr(here, '/');
    const char* backslash = strrchr(here, '\\');
    const char* cut = slash > backslash ? slash : backslash;
    const size_t length = cut == NULL ? 0 : (size_t)(cut - here) + 1;
    if (snprintf(out, capacity, "%.*s../../assets/assets.bin", (int)length, here) >= (int)capacity) {
        return NULL;
    }
    return out;
}

const asset_pack_t*
asset_store_pack(void) {
    static asset_pack_t pack;
    static int state; /* 0 untried, 1 open, -1 failed */
    if (state == 0) {
        char fallback[512];
        const char* env = getenv("AUTANA_ASSET_PACK");
        const char* path = env != NULL ? env : source_pack_path(fallback, sizeof fallback);
        void* buffer = NULL;
        const asset_status_t status = path == NULL ? ASSET_ERR_NO_PACK : asset_file_open(path, &pack, &buffer);
        state = status == ASSET_OK ? 1 : -1;
        if (status != ASSET_OK) {
            (void)fprintf(stderr, "asset pack %s: %s (set AUTANA_ASSET_PACK)\n", path == NULL ? "?" : path,
                          asset_status_text(status));
        }
    }
    return state == 1 ? &pack : NULL;
}

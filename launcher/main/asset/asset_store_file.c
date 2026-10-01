#include <stdio.h>
#include <stdlib.h>

#include "asset/asset_file.h"
#include "asset/asset_store.h"

const asset_pack_t*
asset_store_pack(void) {
    static asset_pack_t pack;
    static int state; /* 0 untried, 1 open, -1 failed */
    if (state == 0) {
        const char* path = getenv("AUTANA_ASSET_PACK");
        void* buffer = NULL;
        const asset_status_t status = path == NULL ? ASSET_ERR_NO_PACK : asset_file_open(path, &pack, &buffer);
        state = status == ASSET_OK ? 1 : -1;
        if (status != ASSET_OK) {
            (void)fprintf(stderr, "asset pack %s: %s (AUTANA_ASSET_PACK names the file build_pack.py wrote)\n",
                          path == NULL ? "(unset)" : path, asset_status_text(status));
        }
    }
    return state == 1 ? &pack : NULL;
}

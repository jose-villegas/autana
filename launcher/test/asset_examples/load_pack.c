/* Load a pack: mount it, find one entry, release it. */
#include "asset/asset_store.h"

asset_status_t
load_pack_example(const char* pack_name, const char* entry, uint32_t type, uint32_t* size) {
    const asset_pack_t* pack = asset_store_pack(pack_name);
    if (pack == NULL) {
        return ASSET_ERR_NO_PACK; /* the log line says why: missing, bad CRC, store full */
    }
    asset_view_t view;
    const asset_status_t status = asset_pack_find(pack, entry, type, &view);
    if (status == ASSET_OK) {
        *size = view.size; /* view.data points into the pack: use it before the release */
    }
    asset_store_release(pack_name);
    return status;
}

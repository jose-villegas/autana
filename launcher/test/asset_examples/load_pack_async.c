/* load_pack_async: request on enter, read each frame, release on exit. */
#include "asset/asset_store.h"

typedef struct {
    const char* name;
    handle_t load;
} load_pack_example_t;

void
load_pack_enter(load_pack_example_t* visit, const char* name) {
    *visit = (load_pack_example_t){.name = name, .load = asset_store_load_async(name, NULL, HANDLE_NONE)};
}

const asset_pack_t*
load_pack_frame(const load_pack_example_t* visit) {
    return asset_store_loaded(visit->load);
}

void
load_pack_exit(load_pack_example_t* visit) {
    if (asset_store_state(visit->load) == PENDING_READY) {
        asset_store_release(visit->name);
    } else {
        asset_store_cancel(visit->load);
    }
    visit->load = HANDLE_NONE;
}

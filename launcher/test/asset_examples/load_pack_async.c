/* load_pack_async: request on enter, read each frame, release on exit. */
#include "asset/asset_store.h"

typedef struct {
    handle_t load;
} load_pack_example_t;

void
load_pack_enter(load_pack_example_t* visit, const char* name) {
    *visit = (load_pack_example_t){.load = asset_store_load_async(name, NULL, HANDLE_NONE)};
}

const asset_pack_t*
load_pack_frame(const load_pack_example_t* visit) {
    return asset_store_loaded(visit->load);
}

void
load_pack_exit(load_pack_example_t* visit) {
    asset_store_release(visit->load);
    visit->load = HANDLE_NONE;
}

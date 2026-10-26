/* Tap: keep the sound in app state until exit. */
#include "asset/asset_store.h" /* asset_store_load, ASSET_ENGINE_PACK */
#include "sound/sound_play.h"

static handle_t tap_load;
static sound_t tap; /* app state, not a stack local */

static bool
tap_start(void) {
    tap_load = asset_store_load(ASSET_ENGINE_PACK);
    if (sound_open(asset_store_loaded(tap_load), "tap", &tap) != ASSET_OK) {
        asset_store_release(tap_load);
        /* no click */
        return false;
    }
    sound_voice_t voice = sound_play(&tap, NULL); /* SFX bus, full gain, once */
    sound_stop(voice);                            /* optional; stale handles are ignored */
    return true;
}

/* Called from app exit(), after the shell's mixer barrier. */
static void
tap_exit(void) {
    asset_store_release(tap_load);
    tap_load = HANDLE_NONE;
}

/* Tap: keep the sound in app state until exit. */
#include "asset/asset_store.h" /* asset_store_load, ASSET_ENGINE_PACK */
#include "sound/sound_play.h"

static sound_t tap; /* app state, not a stack local */

static bool
tap_start(void) {
    if (sound_open(asset_store_load(ASSET_ENGINE_PACK), "tap", &tap) != ASSET_OK) {
        /* no click */
        return false;
    }
    sound_voice_t voice = sound_play(&tap, NULL); /* SFX bus, full gain, once */
    sound_stop(voice);                            /* optional; stale handles are ignored */
    return true;
}

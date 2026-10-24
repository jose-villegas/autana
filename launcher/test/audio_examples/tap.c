/* Tap: keep the sound in app state until exit. */
#include "asset/asset_store.h"
#include "sound/sound_play.h"

static sound_t tap;

static bool
tap_start(void) {
    if (sound_open(asset_store_pack(ASSET_ENGINE_PACK), "tap", &tap) != ASSET_OK) {
        return false;
    }
    sound_voice_t voice = sound_play(&tap, NULL);
    sound_stop(voice);
    return true;
}

/* Tap: keep the sound and its voice in app state until exit. */
#include "asset/asset_store.h" /* asset_store_pack, ASSET_ENGINE_PACK */
#include "sound/sound_play.h"

static sound_t tap;             /* app state, not a stack local */
static sound_voice_t tap_voice; /* kept so a later call can stop it */

static bool
tap_start(void) {
    if (sound_open(asset_store_pack(ASSET_ENGINE_PACK), "tap", &tap) != ASSET_OK) {
        /* no click */
        return false;
    }
    tap_voice = sound_play(&tap, NULL); /* SFX bus, full gain, once */
    return true;
}

/* Stop is a separate call, made later (a second tap, a screen change). Queued straight
 * after sound_play(), both commands reach the mixer before its next block and nothing is heard. */
static void
tap_cancel(void) {
    sound_stop(tap_voice); /* optional; stale handles are ignored */
}

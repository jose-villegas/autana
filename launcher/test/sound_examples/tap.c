/* Tap: open the engine's click once, play it on each tap, stop it in a later call. */
#include "asset/asset_store.h"
#include "sound/sound_play.h"

typedef struct {
    sound_t tap; /* app state, not a stack local: the mixer reads it until the app exits */
    sound_voice_t voice;
} tap_example_t;

/* pack: asset_store_pack(ASSET_ENGINE_PACK) */
void
tap_example_start(tap_example_t* s, const asset_pack_t* pack) {
    if (sound_open(pack, "tap", &s->tap) != ASSET_OK) {
        return; /* no click */
    }
    s->voice = sound_play(&s->tap, NULL); /* SFX bus, full gain, once */
}

/* Stop is a separate call, made later (a second tap, a screen change). Queued straight
 * after sound_play(), both commands reach the mixer before its next block and nothing is heard. */
void
tap_example_cancel(tap_example_t* s) {
    sound_stop(s->voice); /* optional; stale handles are ignored */
}

/* Tap: open the engine's click once, play it on each tap, stop it in a later call. */
#include "asset/asset_store.h"
#include "sound/sound_play.h"

typedef struct {
    handle_t load; /* holds the pack mapped until exit */
    sound_t tap;   /* app state, not a stack local: the mixer reads it until the app exits */
    sound_voice_t voice;
} tap_example_t;

void
tap_example_start(tap_example_t* s) {
    s->load = asset_store_load(ASSET_ENGINE_PACK);
    if (sound_open(asset_store_loaded(s->load), "tap", &s->tap) != ASSET_OK) {
        asset_store_release(s->load);
        s->load = HANDLE_NONE;
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

/* Call from the app's exit(), after the shell's mixer barrier: the pack stays mapped until then. */
void
tap_example_exit(tap_example_t* s) {
    asset_store_release(s->load);
    s->load = HANDLE_NONE;
}

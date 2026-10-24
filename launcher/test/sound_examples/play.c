/* Play a pack sound: open it once, play it on a tap, stop it early. */
#include "asset/asset_store.h"
#include "sound/sound_play.h"

typedef struct {
    sound_t tap; /* app state, not a stack local: the mixer reads it until the app exits */
    sound_voice_t last;
} play_example_t;

void
play_example_open(play_example_t* s) {
    /* A failed open leaves the zeroed sound empty: sound_play() then returns the null voice. */
    (void)sound_open(asset_store_pack(ASSET_ENGINE_PACK), "tap", &s->tap);
}

void
play_example_tap(play_example_t* s) {
    s->last = sound_play(&s->tap, NULL); /* SFX bus, full gain, once; never waits */
}

void
play_example_cancel(play_example_t* s) {
    sound_stop(s->last); /* optional; a stale handle is ignored */
}

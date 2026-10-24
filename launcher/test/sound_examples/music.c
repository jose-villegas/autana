/* Loop music on its own bus, change its volume, stop it. */
#include "asset/asset_store.h"
#include "sound/sound_play.h"

typedef struct {
    sound_t theme; /* app state: the pack and this view outlive the app's last frame */
    sound_voice_t voice;
} music_example_t;

void
music_example_start(music_example_t* s, const asset_pack_t* pack) {
    if (sound_open(pack, "theme", &s->theme) != ASSET_OK) {
        return;
    }
    const sound_play_opts_t opts = {
        .bus = SOUND_BUS_MUSIC, .gain_pct = SOUND_GAIN_MAX_PCT, .priority = SOUND_PRIORITY_NORMAL, .loop = true};
    s->voice = sound_play(&s->theme, &opts);
}

void
music_example_quieter(void) {
    sound_set_bus_gain(SOUND_BUS_MUSIC, SOUND_GAIN_MAX_PCT / 2); /* every music voice, one call */
}

void
music_example_stop(music_example_t* s) {
    sound_stop(s->voice);
}

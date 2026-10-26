/* Loop music on its own bus, change its volume, stop it. */
#include "asset/asset_store.h"
#include "sound/sound_play.h"

typedef struct {
    handle_t load; /* holds the pack mapped until exit */
    sound_t theme; /* app state: the pack and this view outlive the app's last frame */
    sound_voice_t voice;
} music_example_t;

void
music_example_start(music_example_t* s, const char* pack) {
    s->load = asset_store_load(pack);
    if (sound_open(asset_store_loaded(s->load), "theme", &s->theme) != ASSET_OK) {
        asset_store_release(s->load);
        s->load = HANDLE_NONE;
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

/* Call from the app's exit(), after the shell's mixer barrier. */
void
music_example_exit(music_example_t* s) {
    asset_store_release(s->load);
    s->load = HANDLE_NONE;
}

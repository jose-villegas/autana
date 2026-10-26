/* Your own sound: the app's pack holds click.sound.toml baked to entry "click". */
#include "asset/asset_store.h"
#include "sound/sound_play.h"

typedef struct {
    handle_t load; /* holds the pack mapped until exit */
    sound_t click;
    bool have_click;
} own_example_t;

void
own_example_open(own_example_t* s, const char* app_pack) {
    s->load = asset_store_load(app_pack);
    s->have_click = sound_open(asset_store_loaded(s->load), "click", &s->click) == ASSET_OK;
}

void
own_example_confirm(own_example_t* s) {
    if (s->have_click) {
        const sound_play_opts_t opts = {.bus = SOUND_BUS_SYSTEM, .gain_pct = SOUND_GAIN_MAX_PCT};
        (void)sound_play(&s->click, &opts); /* UI feedback follows the shell's UI volume */
    }
}

/* Call from the app's exit(), after the shell's mixer barrier. */
void
own_example_exit(own_example_t* s) {
    asset_store_release(s->load);
    s->load = HANDLE_NONE;
}

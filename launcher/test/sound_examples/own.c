/* Your own sound: the app's pack holds click.sound.toml baked to entry "click". */
#include "asset/asset_store.h"
#include "sound/sound_play.h"

typedef struct {
    sound_t click;
    bool have_click;
} own_example_t;

void
own_example_open(own_example_t* s, const char* app_pack) {
    s->have_click = sound_open(asset_store_pack(app_pack), "click", &s->click) == ASSET_OK;
}

void
own_example_confirm(own_example_t* s) {
    if (s->have_click) {
        const sound_play_opts_t opts = {.bus = SOUND_BUS_SYSTEM, .gain_pct = SOUND_GAIN_MAX_PCT};
        (void)sound_play(&s->click, &opts); /* UI feedback follows the shell's UI volume */
    }
}

# Sound how-to

Play sounds from an app: open a sound from a pack, play it, put it on a bus,
loop it, add your own. Every code block is a compiled, tested file; the rules
behind it are in the [Sound mixer](Sound-Mixer.md) reference.

| I want to | Go to |
|---|---|
| hear a click on a tap | [Play a sound](#play-a-sound) |
| ship my own sound | [Add your own sound](#add-your-own-sound) |
| choose where it plays | [Pick a bus](#pick-a-bus) |
| run music and change its volume | [Loop and stop](#loop-and-stop) |
| avoid a crash at exit | [Keep it alive until the app exits](#keep-it-alive-until-the-app-exits) |

## Play a sound

A pack is the read-only bundle of baked content in flash
([Asset packs](../assets/README.md)). The engine pack ships a `tap` click any app
may use. Open it once, play it by pointer.

<!-- generated: sound-play-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
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
```
<!-- /generated: sound-play-example -->

`sound_play()` returns a handle at once and the mixer starts the sound within one
block, about 5.8 ms. After a quiet spell the first click starts about 250 ms late
while the amplifier warms up ([Amplifier](Mixer-Internals.md#amplifier)).

## Add your own sound

| Step | What you do |
|---|---|
| 1. Source | put `click.wav` (WAV, FLAC, OGG or MP3, any channel count) in your app's pack folder |
| 2. Recipe | beside it, `click.sound.toml` with `source = "click.wav"` ([Sound assets](Sound-Assets.md)) |
| 3. Bake | the build bakes it into entry `click` of your app's pack ([Packs](../assets/README.md#packs)) |
| 4. Open | `sound_open(asset_store_pack(...), "click", &state)` as below |

<!-- generated: sound-own-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
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
```
<!-- /generated: sound-own-example -->

A sound costs no RAM beyond its small `sound_t`: samples are read from the pack in flash.

| Format | Pack size per second | Example: 220 ms click |
|---|---|---|
| PCM16, 22,050 Hz mono | about 44 KB | about 9.7 KB |
| IMA-ADPCM (4:1 compressed, slightly lossy) | about 11 KB | about 2.5 KB (the engine `tap`) |

## Pick a bus

| Sound | Bus |
|---|---|
| UI feedback: taps, clicks, confirmations | `SOUND_BUS_SYSTEM`, so the shell's UI volume applies |
| Game effects | `SOUND_BUS_SFX`, the default |
| Music | `SOUND_BUS_MUSIC` |

Pass the bus in `sound_play_opts_t`. NULL options mean SFX, full gain,
`SOUND_PRIORITY_NORMAL` and no loop. Gain rules: [Buses and gain](Sound-Mixer.md#buses-and-gain).

## Loop and stop

<!-- generated: sound-music-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
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
```
<!-- /generated: sound-music-example -->

A sound with loop points loops inside them without `opts.loop`. How loops and
loop points interact: [Loops](Sound-Mixer.md#loops).

## Keep it alive until the app exits

`sound_play()` stores your pointer and copies no samples, so the `sound_t` and its
pack must stay valid until the app exits. That is why the examples keep it in app state.

| Event | Frees the sound? |
|---|---|
| `sound_stop()`, a steal, `sound_is_playing()` returning false | no: the mixer may read it for up to one block |
| App exit | yes, safely: the shell stops your voices and waits for the mixer first ([App exit](Mixer-Internals.md#app-exit)) |

## Try it from a terminal

`autana sound play tap` plays the engine sound on a development build. The
commands are in [Drive](../cli/Drive.md).

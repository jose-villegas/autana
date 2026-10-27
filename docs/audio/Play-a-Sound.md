# Play a sound

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
([Asset packs](../assets/README.md)). The engine pack, `asset_store_load(ASSET_ENGINE_PACK)`,
ships a `tap` click any app may use. Load it once into app state, play it by pointer, stop it if you need to,
and release the load at exit.

<!-- generated: sound-tap-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
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
```
<!-- /generated: sound-tap-example -->

Keep the handle and stop in a later call, as `tap_example_cancel` does. Stop right after play and both commands reach
the mixer before its next block, so nothing is heard.

`sound_play()` returns a handle at once and the mixer starts the sound within one
block, about 5.8 ms. After a quiet spell the first click starts about 250 ms late
while the amplifier warms up ([Amplifier](Mixer-Internals.md#amplifier)).

## Add your own sound

| Step | What you do |
|---|---|
| 1. Source | put `click.wav` (WAV, FLAC, OGG or MP3, any channel count) in your app's pack folder, the one holding its `NAME.pack.toml` |
| 2. Recipe | beside it, `click.sound.toml` with `source = "click.wav"` and `format = "adpcm"` or `"pcm16"` ([Sound assets](Sound-Assets.md)) |
| 3. Bake | the build bakes it into entry `click` of that folder's pack ([Packs](../assets/Building-Packs.md#packs)) |
| 4. Open | `asset_store_load(pack)`, then `sound_open(asset_store_loaded(load), "click", &state)` as below, and release the load at exit |

<!-- generated: sound-own-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
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
| Tactile case pulses | `SOUND_BUS_HAPTIC`, through [haptic_pulse()](../haptics/README.md) |

Pass the bus in `sound_play_opts_t`. NULL options mean SFX, full gain,
`SOUND_PRIORITY_NORMAL` and no loop. Gain rules: [Buses and gain](Sound-Mixer.md#buses-and-gain).

## Loop and stop

<!-- generated: sound-music-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
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
```
<!-- /generated: sound-music-example -->

`opts.loop` loops a sound inside its loop points, or from frame 0 when it has none. All cases: [Loops](Sound-Mixer.md#loops).

## Keep it alive until the app exits

`sound_play()` stores your pointer and copies no samples, so the `sound_t` and its
pack must stay valid until the app exits. That is why the examples keep both the `sound_t` and the load handle in app state, and release the load in `exit()`.

| Event | Frees the sound? |
|---|---|
| `sound_stop()`, a steal, `sound_is_playing()` returning false | no: the mixer may read it for up to one block |
| App exit | yes, safely: the shell stops your voices and waits for the mixer first ([App exit](Mixer-Internals.md#app-exit)) |

## Try it from a terminal

`autana sound play tap` plays the engine sound on a development build. The
commands are in [Drive](../cli/Drive.md).

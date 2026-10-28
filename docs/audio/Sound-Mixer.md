# Sound mixer

The sound mixer plays [stored sounds](Sound-Assets.md) on the speaker: an app asks
for a sound, and one background task mixes every playing sound into the codec (the
audio chip that drives the speaker). The app API is `sound/sound_play.h`; the shell
and boot use `sound/sound_engine.h`. This page is the reference. To get a sound
playing, start with [Play a sound](Play-a-Sound.md).

| Page | Reader |
|---|---|
| [Play-a-Sound.md](Play-a-Sound.md) | app author: tested steps |
| Sound-Mixer.md (this page) | app author: voices, handles, buses, loops |
| [Mixer-Internals.md](Mixer-Internals.md) | engine developer: task, queue, amplifier, exit |
| [Mixer-Cost.md](Mixer-Cost.md) | performance work: measured cost and RAM |
| [Clicks.md](Clicks.md) | audio tuning: de-click and onsets |

## Calls

| Call | Does |
|---|---|
| `sound_play(sound, opts)` | start a voice; returns its handle, or the null voice when it cannot |
| `sound_stop(voice)` | stop it |
| `sound_set_gain(voice, pct)` | scale that voice |
| `sound_is_playing(voice)` | whether the handle is still live |
| `sound_bus_create(name)` / `sound_bus_destroy(bus)` | custom buses |
| `sound_set_bus_gain(bus, pct)` | scale a bus |

The engine API in `sound_engine.h` provides
`sound_play_tone(uint16_t hz, uint16_t ms, uint8_t gain_pct, sound_bus_t bus)`.

A tone plays on the given bus for `audio_out_samples(ms)` frames, then ends
through the one-shot de-click tail. `sound_is_playing()` becomes false at that end.

`SOUND_TONE_UNTIL_STOPPED` keeps the tone playing until stopped. The console
`tone` and `sound` commands use it on `SOUND_BUS_SYSTEM` and stop their own voices.

## Voices and buses

A voice is one playing sound, and a bus is a volume group that voices play
through. The limits are named in `sound_play.h`.

### Handles and stealing

Each voice slot keeps a voice generation: a counter it bumps every time the
slot is reused, so an old handle can never reach a newer sound in its slot.

| Rule | Behaviour |
|---|---|
| Capacity | 8 voices at once (`SOUND_VOICE_COUNT`); 32 is the measured ceiling, where present loses about 1 ms per frame ([Cost](Mixer-Cost.md)) |
| Handle | `sound_voice_t.id` encodes the slot and its voice generation; 0 is the null voice |
| Stale handle | after a stop or a steal, stop and gain calls do nothing and `sound_is_playing()` is false |
| Full table | a new play always steals: the lowest priority voice, then the oldest among equals |
| Null options | SFX bus, full gain, `SOUND_PRIORITY_NORMAL`, no loop |
| Pointer lifetime | `sound_play()` keeps your `sound_t` pointer: keep it and its pack valid until app exit ([Play a sound](Play-a-Sound.md#keep-it-alive-until-the-app-exits)) |

### Buses and gain

| Bus | Purpose |
|---|---|
| `SOUND_BUS_SYSTEM` | UI sounds; one volume for the shell and every app |
| `SOUND_BUS_SFX` | game sound effects; the default |
| `SOUND_BUS_MUSIC` | music |
| `SOUND_BUS_HAPTIC` | speaker-driven [haptic pulses](../haptics/README.md), independent of UI volume |
| custom | `sound_bus_create(name)`, up to `SOUND_BUS_COUNT_MAX` buses in all; invalid handle when full |
| `SOUND_BUS_MASTER` | gain only: scales every bus |

Every bus feeds master directly, with no nesting. Gains are percents up to
`SOUND_GAIN_MAX_PCT`, and out-of-range values are ignored.

Each sample is scaled by
`voice gain x bus gain x master gain`, folded into one fixed-point factor per voice
per block, so a sample costs one 32-bit multiply and shift.

The codec's own volume is fixed at `AUDIO_OUT_CODEC_VOLUME` in `audio_out.c`; apps
never change it. Destroying a bus stops its voices and frees its slot. The three
built-in buses are never destroyed.

### Loops

| Sound | `opts.loop` | Result |
|---|---|---|
| has loop points | set | loops inside them (`sound_read()` wraps) |
| none | set | restarts from frame 0 |
| has loop points | clear | stops at `loop_end`, the cut de-clicked ([Clicks](Clicks.md)) |

## When it does nothing

| Cause | Result |
|---|---|
| Open failed, empty sound, or sound off | `sound_play()` returns the null voice |
| Command queue full | the command is dropped ([Queue and overflow](Mixer-Internals.md#queue-and-overflow)) |
| Called during the app's `exit()` | changes nothing ([App exit](Mixer-Internals.md#app-exit)) |
| Codec or task failed at boot | sound is off, boot continues ([Initialization failure](Mixer-Internals.md#initialization-failure)) |
| First click after 2 s of quiet | plays about 250 ms late: the amplifier warms up ([Amplifier](Mixer-Internals.md#amplifier)) |

The terminal commands (`autana sound play`, `stop`, `stats`, `onset`) are in
[Drive](../cli/Drive.md).

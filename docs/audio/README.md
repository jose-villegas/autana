# Audio

Audio spans the platform codec and decoder in `launcher/main/audio/` and the
engine sound assets, cursors and mixer in `launcher/main/sound/`. These docs cover
both layers and their measurements. The board's audio wiring is in the
[board notes](../notes/Board-and-Memory.md#audio).

| Reference | Scope |
|---|---|
| [Audio-Path.md](Audio-Path.md) | codec and I2S lifetime, the development loopback check, captured RAM and level measurements |
| [Sound-Assets.md](Sound-Assets.md) | the SOND pack entry, its baker and reader, and the cursor that reads samples out |
| [Sound-Mixer.md](Sound-Mixer.md) | task and queue, playback API, voices, buses, amplifier states and internal RAM |

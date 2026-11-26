# Audio

Audio spans the platform codec and decoder in `launcher/main/audio/` and the
engine sound assets, cursors and mixer in `launcher/main/sound/`. These docs cover
both layers and their measurements. The board's audio wiring is in the
[board notes](../notes/Board-and-Memory.md#audio).

| Reference | Scope |
|---|---|
| [Audio-Path.md](Audio-Path.md) | codec and I2S lifetime, the development loopback check, captured RAM and level measurements |
| [Haptics](../haptics/README.md) | speaker-driven case pulses and bus |
| [Low-Frequency-Buzz.md](Low-Frequency-Buzz.md) | low tones as a felt buzz: board shake (IMU) against loudness (mic), 20-300 Hz |
| [Sound-Assets.md](Sound-Assets.md) | the SOND pack entry, its baker and reader, and the cursor that reads samples out |
| [Play-a-Sound.md](Play-a-Sound.md) | tested steps: play a sound, add your own, pick a bus, loop and stop, keep it alive |
| [Sound-Mixer.md](Sound-Mixer.md) | the playback API: voices, handles and stealing, buses and gain, loops |
| [Mixer-Internals.md](Mixer-Internals.md) | task and queue, locking, amplifier states, app exit, capture, init failure |
| [Mixer-Cost.md](Mixer-Cost.md) | generated cost, frame time and internal RAM tables, and how to measure them |
| [Clicks.md](Clicks.md) | the de-click ramp, start, stop, steal and amplifier onsets, and their plots |

# Audio path

Part of the [audio docs](README.md). The board's audio wiring (amplifier pin, amplifier
startup, microphone gain) is in the [board notes](../notes/Board-and-Memory.md#audio).

## Path and lifetime

`sound_init()` opens the reference-counted `audio_codec` through `audio_out`
and keeps TX DMA for the firmware lifetime. `sound_mixer_device.c` owns the
only writer. Playback commands, voices, buses and the amplifier state machine
are described in [Sound-Mixer.md](Sound-Mixer.md).

RX is created and initialized only for a capture. Capture open and close run
as mixer commands: TX pauses around RX creation and teardown, then resumes.
The ESP32-S3 codec adapter defers RX shutdown while paired TX is active, so
teardown requests TX shutdown through that adapter before closing RX. Voice
positions hold across both capture operations.

Each direction uses an `AUDIO_CODEC_DMA_BLOCKS` x `AUDIO_CODEC_BLOCK_SAMPLES`
DMA ring (`launcher/main/audio/audio_codec.h`). PCM is mono, 16-bit at
`AUDIO_OUT_SAMPLE_RATE_HZ` (`launcher/main/audio/audio_out.h`). IDF allocates
the ring in `i2s_channel_init_std_mode()`, rather than `i2s_new_channel()`.
The codec, control and GPIO interfaces have one shared bring-up. The BSP's
`bsp_audio_*` helpers are unused because they have no deinit.

Signal path during a loopback check:

```mermaid
flowchart TB
    subgraph Play["Out: I2S to the speaker"]
        direction LR
        Tx["I2S TX<br/>DMA"] --> Dac["ES8311<br/>DAC"] --> Amp["power<br/>amplifier"] --> Speaker["speaker"]
        Enable["board_audio_amp_enable()<br/>on GPIO 46"] -. enables .-> Amp
    end
    subgraph Capture["In: microphone to I2S"]
        direction LR
        Mic["microphone"] --> Adc["ES8311<br/>ADC"] --> Rx["I2S RX<br/>DMA"]
    end
    Codec["audio_codec"] -. configures the DAC over I2C .-> Play
    Play -->|over the air| Capture
    Codec -. configures the ADC over I2C .-> Capture
```

The [volume table](#level-vs-volume) shows compression at high volume.
The [duration table](#level-vs-duration) shows that waiting for amplifier
startup keeps short tones at the same level as long tones.

## Development check

Run `autana tone <hz> <ms> [volume] [--internal] [--json]`, or the console verb
`tone <hz> <ms> [volume] [internal]`.

`console_verb_tone()` opens capture, measures its DMA-capable heap cost, runs
the check, and closes capture
(`launcher/main/console/verbs/console_tone.c`).

The console uses `audio_loopback_measure()` for a baseline, starts a SYSTEM-bus
tone through the mixer, measures again, then stops the voice. It waits for
amplifier warm-up before measuring. A Goertzel filter measures the requested frequency.<sup>[[72]](../Citations.md#72)</sup>

`--internal` routes DAC data to both ADCDAT slots inside the ES8311. It holds
the speaker amplifier off for the whole check. The verb restores the exact register
read-back and releases the hold on every exit, before closing capture.
Both modes refuse to measure until any pending register restore succeeds.

```mermaid
flowchart LR
    Tx["I2S TX"] --> Dac["ES8311 DAC"] --> Dat["ES8311 ADCDAT<br/>both slots"] --> Rx["I2S RX"]
    Dac -.-> Amp["amplifier held off"]
```

Each window's Goertzel phase<sup>[[72]](../Citations.md#72)</sup> is referenced to its first sample.

The wrapped difference between consecutive windows' phase advances and the
nominal advance gives the frequency offset, as in the phase vocoder<sup>[[82]](../Citations.md#82)</sup>,
here from a single bin rather than a filter bank.

The unambiguous offset range is ±sample rate / (2 × window samples). The reply
reports `path` and `measured_hz`; JSON adds `hz_error` in Hz, measured minus
requested.

The estimate is valid only while the tone stays within the unambiguous range
above; a drifting or modulated tone gives a wrong value.

Below `AUDIO_FREQUENCY_MIN_SAMPLES`, the reply says `measured n/a`
and both JSON frequency fields are null.

The detector discards `AUDIO_LOOPBACK_SETTLE_MS` of capture before measuring.
Heard means tone minus silence is at least `AUDIO_LOOPBACK_HEARD_MARGIN_DB`
(`launcher/main/audio/audio_loopback.h`). Playback and capture buffers have
block size, independent of tone duration.

An acoustic heard result checks I2S in both directions, codec DAC and ADC,
amplifier, speaker and microphone. Internal feedback checks digital playback
and capture inside the codec.

`open_dma_bytes` reports the DMA-capable heap consumed by capture while TX
is open. Capture blocks the console task, away from the frame loop. Stop
other voices when measuring the speaker alone.

## Captured measurements

Tables are generated from the committed capture, not edited by hand.
Regenerate with `python launcher/tools/audio/audio_doc_tables.py --write`;
`--check` rejects stale tables. Levels are frequency-selective dBFS from the
microphone, not calibrated sound pressure levels.

### RAM

The table below is a capture of the stated board and image. Use the
[permanent mixer budget](Sound-Mixer.md#internal-ram) for boot-owned allocations;
a live tone reply's `open_dma_bytes` accounts for capture only.

<!-- generated: audio-ram sha256=570448eed63ae5ac5c6c9e10892409d2c16beff419df3a083348d33e0e41ab49 -->
> Generated by: `python launcher/tools/audio/audio_doc_tables.py --write`

Board **A308**, build **2521fd82ef55-dev** ([raw replies](data/audio_capture.txt)).

| State | Free DMA heap (B) | open_dma_bytes (B) | Kept vs idle (B) | Largest DMA block (B) |
|---|---|---|---|---|
| Idle (first heap) | 98223 | n/a | n/a | 65536 |
| Open (first tone) | 95127 | 3096 | n/a | not captured |
| Open (later tones) | not captured | 3040 | n/a | not captured |
| After close (last heap) | 98167 | n/a | 56 | 63488 |

First-open free heap is idle minus `open_dma_bytes`; later opens report only the delta.
<!-- /generated: audio-ram -->

### Level vs frequency

<!-- generated: audio-frequency sha256=03a8309737aedb2882c4afe9e63753d8b9555352584fb4fab0dabfddbf490cd9 -->
> Generated by: `python launcher/tools/audio/audio_doc_tables.py --write`

Board **A308**, build **2521fd82ef55-dev** ([raw replies](data/audio_capture.txt)).

Duration 500 ms; console default volume.

| Frequency (Hz) | Tone (dBFS) | Silence (dBFS) | Margin (dB) | Heard |
|---|---|---|---|---|
| 300 | -31.2 | -110.3 | 79.1 | yes |
| 1000 | -14.9 | -117.4 | 102.5 | yes |
| 3000 | -30.0 | -120.0 | 90.0 | yes |
<!-- /generated: audio-frequency -->

### Level vs volume

<!-- generated: audio-volume sha256=16d9ba57837baf7367414854b703d1ec886612cf872e956f87111612bc5fb6bb -->
> Generated by: `python launcher/tools/audio/audio_doc_tables.py --write`

Board **A308**, build **2521fd82ef55-dev** ([raw replies](data/audio_capture.txt)).

Frequency 1000 Hz; duration 500 ms.

| Volume (%) | Tone (dBFS) | Silence (dBFS) | Margin (dB) | Heard |
|---|---|---|---|---|
| 1 | -42.5 | -120.0 | 77.5 | yes |
| 3 | -33.0 | -120.0 | 87.0 | yes |
| 10 | -22.5 | -120.0 | 97.5 | yes |
| 30 | -14.9 | -120.0 | 105.1 | yes |
| 100 | -14.1 | -117.9 | 103.8 | yes |
<!-- /generated: audio-volume -->

### Level vs duration

<!-- generated: audio-duration sha256=95188d4afe82134cf5e06e574c0e666414fccd34171c3e639dadaab0fe2590f9 -->
> Generated by: `python launcher/tools/audio/audio_doc_tables.py --write`

Board **A308**, build **2521fd82ef55-dev** ([raw replies](data/audio_capture.txt)).

Frequency 1000 Hz; console default volume.

| Duration (ms) | Tone (dBFS) | Silence (dBFS) | Margin (dB) | Heard |
|---|---|---|---|---|
| 20 | -14.8 | -89.0 | 74.2 | yes |
| 50 | -14.8 | -95.2 | 80.4 | yes |
| 100 | -14.9 | -100.9 | 86.0 | yes |
| 200 | -14.9 | -108.3 | 93.4 | yes |
| 500 | -14.9 | -117.4 | 102.5 | yes |
<!-- /generated: audio-duration -->

## Constraint for held-open audio

While audio is held open, the largest single contiguous internal DMA
allocation any caller needs must still be satisfiable. The power-on
self-test checks a floor for the largest DMA block, owned by
`MIN_LARGEST_DMA_BLOCK` in `launcher/main/selftest/post.c`. Audio buffers
must not fragment the largest block below that requirement. The [RAM table](#ram)
identifies its capture image. `autana heap` reports the headroom with the
mixer running.

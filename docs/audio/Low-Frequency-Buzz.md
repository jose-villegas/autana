# Low-frequency buzz

Can a low tone shake the board enough to feel while it stays barely audible?
This page holds the sweep that answers it: board shake from the IMU against
loudness from the microphone, for tones from 20 to 300 Hz.

The coupling capacitor and resistor ahead of the amplifier make a high-pass
near 10 Hz, so every swept tone reaches the speaker. The 1 W speaker bounds
the loudest a tone may be held: see Safety.

## How a point is measured

`autana tone <hz> <ms> <volume>` measures twice: once over silence, once while
the tone plays. In development builds its reply also carries the board's
shake over both windows.

| Quantity | Source | Detail |
|---|---|---|
| Shake (mg RMS) | IMU accelerometer FIFO, read at the end of each window | Spread of each sample around the window's mean. The mean holds gravity and tilt, so only shaking remains |
| IMU rate | `IMU_BUZZ_ODR_CODE` during the check, `IMU_ODR_CODE` after | The sensor's default bandwidth is half its rate, so the normal rate is blind above about 110 Hz; the check runs it 4x faster |
| Window | the newest `IMU_FIFO_SAMPLES` samples of the tone | Long enough for three cycles of the lowest tone |
| Silence (mg RMS) | the same reading over the silent window | The noise floor; shake counts only at twice this |
| Tone, THD (dBFS, dB) | the loopback Goertzel at the tone and its harmonics | As in [Audio-Path.md](Audio-Path.md) |
| Audible (dBFS, A) | tone plus harmonics, A-weighted<sup>[[87]](../Citations.md#87)</sup> | Harmonic power is taken as all at the second harmonic. A small cone is heard mostly through its harmonics at low frequency |
| Score (dB) | 20 log10 of the shake above the floor, minus the audible level | Higher means more shake for the same loudness |

The microphone's own low-frequency roll-off is not corrected, so the
audible level of the lowest tones reads low. The hand check below covers it.

## Safety

The sweep holds tones for at most 500 ms, with a rest between them.
Below 60 Hz it stays at 60% volume or less, since low tones move the cone
furthest. The full-volume point is one burst per frequency, at 60 Hz or above.

## Results

<!-- generated: audio-buzz sha256=a93006c90ebb47c04a5a941a4f16d5cd1d65ec2d7326a2c3e4506015b15e8a1c -->
No capture yet.
<!-- /generated: audio-buzz -->

## Run it

Board flat on a desk, untouched, in a quiet room, on a screen that reads no
motion (the launcher home's ridge reacts to shaking):

```sh
autana --wait 3600 lock run -- python launcher/tools/audio/audio_buzz_sweep.py --board <id>
python launcher/tools/audio/audio_doc_tables.py --write
```

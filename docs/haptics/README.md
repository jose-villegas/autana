# Haptics

The speaker shakes the case. Every pulse is also a soft sound; the
[low-frequency measurements](../audio/Low-Frequency-Buzz.md) describe the
resonance and usable gains.

## API and rules

Include `haptics/haptics.h`. The API `sound_voice_t haptic_pulse(haptic_t strength)`
returns the new voice or `SOUND_VOICE_NULL` if playback could not start.

```c fragment
if (ui_button(ui, "Start")) {
    haptic_pulse(HAPTIC_FIRM);
}
```

| Rule | Behavior |
|---|---|
| Strength | `HAPTIC_LIGHT` uses `HAPTIC_LIGHT_GAIN_PCT` (30%); `HAPTIC_FIRM` uses `HAPTIC_FIRM_GAIN_PCT` (60%) |
| Length | `HAPTIC_FIRM_MS` (100 ms) is the shortest pulse whose median shake reaches 90% of the 400 ms tone's in every run. `HAPTIC_LIGHT_MS` uses the same duration at lower gain. |
| Frequency | `HAPTIC_HZ` (202 Hz), the measured case resonance |
| Overlap | a new pulse stops the previous pulse and restarts its duration |
| Bus | `SOUND_BUS_HAPTIC`, independent of the SYSTEM UI volume; its gain resets on app exit |
| Mute | master mute and HAPTIC bus mute silence pulses |
| Music | pulses mix on top at `SOUND_PRIORITY_MAX`; loud music can reduce the pulse through the codec limiter |

The constants have one owner in `launcher/main/haptics/haptics.h`. Playback uses
the mixer's timed tone, bus gains and one-shot de-click tail.

## Development measurement

On a development console, `haptic light` or `haptic firm` replies
`HAPTIC <strength> audible_ms=<ms>`, measured from the pulse call until
`sound_voice_wait_audible()` confirms audibility. A failed start or timeout
replies with an error.

## Removable in one pass

The feature remains removable until a pulse is confirmed felt and acceptable.
Remove these pieces together:

| Piece | Location |
|---|---|
| Pulse module and development verb | `launcher/main/haptics/`, its CMake entry and `suite_haptics.c` |
| Built-in bus | `SOUND_BUS_HAPTIC`, built-in count and name in `launcher/main/sound/` |
| Registration and verification | haptics layer entry, host tests and documentation links |

# Haptics: design sketch

Tactile feedback with the speaker as the actuator. The board has no vibration
motor; the speaker's tone shakes the case. Measured in
[Low-Frequency-Buzz.md](../audio/Low-Frequency-Buzz.md): nothing shakes the
board silently, so every pulse is also a soft sound. The case resonance near
202 Hz gives the most feel per loudness; 30% and 60% are felt, 10% is not, and
the codec limiter caps any gain above 60%.

Status: sketch, not built. Removable in one pass until a pulse is shown felt
and acceptable on the board.

## 1. Owner

No feedback or notify module exists today (searched `ui/`, `shell/`, `input/`,
`services/`, `sound/`). UI sounds are only a bus (`SOUND_BUS_SYSTEM`).

| Piece | Where |
|---|---|
| `haptics/haptics.h`, `haptics.c` | `launcher/main/haptics/`, engine row, above `sound/`, beside `ui/` |
| Docs | `docs/haptics/README.md` |
| Depends on | `sound/sound_engine.h` (`sound_play_tone`), nothing in `ui/` or `input/` |
| Used by | `ui/` controls and `shell/`; apps through `haptics.h` |

## 2. API

```c fragment
typedef enum { HAPTIC_LIGHT, HAPTIC_FIRM } haptic_t;

void haptic_pulse(haptic_t strength);
```

```c fragment
if (ui_button(ui, "Start")) {
    haptic_pulse(HAPTIC_FIRM);
}
```

| Rule | Detail |
|---|---|
| Strengths | named, not percents: the measured range is two steps wide (30% felt, 60% at the limiter). A third name only if a board test shows one |
| Length | fixed per strength (`HAPTIC_LIGHT_MS`, `HAPTIC_FIRM_MS`), set by milestone 2's hand test; not an argument |
| Frequency | `HAPTIC_HZ`, one engine constant (202 Hz on A308) until the other boards are swept |
| Overlap | a new pulse restarts the voice; never stacks |
| Bus | its own built-in `SOUND_BUS_HAPTIC`, so a haptics setting does not ride the UI volume (at a low UI volume a pulse stops being felt but still sounds) |
| Master muted | no pulse: it cannot be silent, so mute wins |
| Music playing | the pulse mixes on top at top priority; the limiter may lower it under loud music (accepted, noted) |

## 3. Mixer change

`sound_play_tone(hz, gain_pct)` plays until stopped. It gains a length and a bus;
no parallel call.

```c fragment
#define SOUND_TONE_UNTIL_STOPPED 0

sound_voice_t sound_play_tone(uint16_t hz, uint16_t ms, uint8_t gain_pct, sound_bus_t bus);
```

| Part | Change |
|---|---|
| Command | the tone payload becomes `{uint16_t hz; uint16_t ms;}` inside the existing union; `SOUND_COMMAND_BYTES` unchanged |
| Mix voice | the tone source keeps `frames_left`; `read_voice` returns at most that, then `playing = false` |
| End | the timed tone ends through the same de-click tail as a one-shot sound: `voice_at_cut()` loses its tone exception |
| Control | `sound_is_playing()` turns false once the mixer reports it, as for one-shots |
| Callers | console `tone` and `sound` verbs pass `SOUND_TONE_UNTIL_STOPPED` and `SOUND_BUS_SYSTEM` |

## 4. Per-board resonance

Only A308 is measured. A narrow sweep on 20E8 and 2124 decides whether
`HAPTIC_HZ` is one constant:

```sh
autana --wait 3600 lock run -- python launcher/tools/audio/audio_buzz_sweep.py --board <id> --hz 150,160,170,180,190,200,210,220,230,240,250 --volumes 10,30,60 --loud 0
```

No hand test: the IMU reading stands in for feel. The reference is A308's
lowest 30% run at 202 Hz, the weakest reading that was felt (7.45 mg RMS in
the capture); its 10% runs (3.2-3.6 mg) were not felt.

| Outcome | Decision |
|---|---|
| each board's 30% median at 202 Hz reaches A308's felt reading | keep `HAPTIC_HZ` = 202 |
| one falls short, but one swept frequency reaches it on all three | move `HAPTIC_HZ` there |
| no shared frequency reaches it | stop: per-board tuning is a new design, back to the maintainer |

## 5. Shake ridge, power, latency, call sites

| Topic | Finding / plan |
|---|---|
| Shake ridge (autana-9zf2y) | a 60% pulse at 202 Hz is 9 mg RMS, 16 mg worst peak: below the ridge's ~94 mg threshold today and the 500 mg shake reader it moves to (#1140). Tilt readers see it as noise against 1 g. No gate needed |
| Amplifier latency | the amp is off after 2 s idle and warms for ~250 ms, so a pulse after a quiet spell lands ~250 ms late, too late for a tap. Recommended: `haptics` asks the mixer to warm the amp on touch-down (one new call on `sound_engine.h`), so the commit's pulse is on time. Milestone 2 measures the cold delay and the amp's idle current first |
| Power | the amplifier stays on while the screen is on; pulses add speaker drive |
| Call sites (UI orchestrator decides) | button and toggle confirm, long-press threshold, slider detents, home-edge swipe commit (`shell_home_edge`), list end stop, a refused action |

## 6. Milestones

| # | Milestone | Needs | Done when |
|---|---|---|---|
| 1 | Timed tone: `sound_play_tone` gains `ms` and `bus` | #1216 merged | suites cover end-on-time, de-click and the bus; console verbs behave as before |
| 2 | Short pulses on A308: shake window sized to the pulse, then 202 Hz at 30/60% x 30/60/100/200 ms, a cold-amp pulse, the amp's idle current | 1, #1176 merged | a generated table; each strength's length is the shortest that reaches the long tone's shake |
| 3 | Sweep 20E8 and 2124 | #1176 merged | `HAPTIC_HZ` decided by the IMU rule in section 4 |
| 4 | `haptics/` module, `SOUND_BUS_HAPTIC`, amp warm on touch-down, `docs/haptics/` | 2, 3 | `haptic_pulse()` on host and board; first pulse after idle on time; still removable |
| 5 | First call site, chosen by the UI orchestrator | 4 | the maintainer calls the pulse felt and acceptable; the removability note goes |

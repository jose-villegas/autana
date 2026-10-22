# Shake round

Input Lab's development shake page follows motion in the BOOT cycle.
It stays in default landscape with panel-axis measurements.
Tap to start; BOOT leaves the page and discards the transcript.

Linear x/y/z and SIZE show acceleration minus estimated down, in mg.
SMOOTH filters SIZE; JERKY filters the size after removing a slow vector average.
OLD shows `tilt_shake()` on its 0–255 scale, updated once per frame.

`shake_probe.h` owns the protocol duration, filter constants and bar scale.
SIZE, SMOOTH and JERKY use that mg bar scale; OLD uses its full range.
Candidates continue updating outside a capture round.

## Guided capture

Disconnect USB before starting so the cable cannot constrain motion.
Each step lasts `SHAKE_PROBE_STEP_MS`. White flashes mark step changes;
a longer cyan flash marks completion. Taps during the round do not restart it.

| Step | Action |
|---|---|
| STILL | Leave it on the table |
| HELD | Hold it as in play |
| GENTLE | Shake it gently |
| HARD | Shake it hard |
| WALK | Walk while holding it |
| TURN | Turn in place |
| FLIP | Tilt and flip it quickly |
| TAP | Tap and drag the screen |

Keep the page open, reconnect USB and capture the retained transcript:

```sh
autana labdump > shake.log
python launcher/main/apps/input_lab/tools/shake_fit.py shake.log --plot shake.png
```

The dump frees the retained PSRAM. Reset, leaving the page or starting another
round also discards it. Allocation failure displays `NO MEMORY FOR ROUND`.

## Transcript and analysis

A `round shake` header precedes `shake_step` edges and per-sample `shake` lines.
Step numbers follow `shake_probe_step_t`; timestamps are round-relative ms,
estimated from drain time and the sample period. Lost samples are not synthesized.

Each sample contains calibrated `acc` and gravity-subtracted `lin` vectors in mg,
then SIZE, SMOOTH, JERKY and the current frame's OLD value.
Gravity consumes each sample separately; only the first slice carries gap/loss metadata.

The script reports median, p95, p99 and maximum for each step and candidate.
The proposed threshold is the largest p99 among STILL, HELD, WALK, TURN, FLIP and TAP;
it reports the margin to GENTLE's median and HARD's p95 as a full level.

The PNG plots candidates over time, coloured by step, with each y-axis starting
at zero. A negative margin means that candidate does not separate the protocol's
non-shake actions from gentle shaking at the proposed threshold.

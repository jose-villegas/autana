# Shell orientation

```mermaid
flowchart LR
    subgraph Filter[Sample]
        direction TB
        A[Screen-axis accel] --> B[Reset or low-pass]
        B --> C[Track motion events]
    end
    subgraph Predict[Predict]
        direction TB
        D{Tilt accepted?} -->|yes| F[Nearest quarter]
        F --> K{Hysteresis passed?}
        D -->|no| E[Clear prediction]
        K -->|no| E
        K -->|yes| L[Keep prediction timer]
    end
    subgraph Commit[Commit]
        direction TB
        G{Settled and cooldowns clear?} -->|yes| I{No touch?}
        G -->|no| H[Keep quarter]
        I -->|no| H
        I -->|yes| J[Commit quarter]
    end
    Filter --> Predict --> Commit
```

The shell samples acceleration every `DISPLAY_SAMPLE_MS` and runs the
Android WindowOrientationListener judge [75](../Citations.md#75).
`display_quarter_now()` returns the committed quarter.

Screen axes point right, down and out. Down at rest is negative z face-up,
so tilt uses `-z / magnitude`: positive means face-up.
The filter and gates use floats, square roots and precomputed angle companions.

| Constant in `display/display.h` | Value | Source |
|---|---:|---|
| `DISPLAY_SAMPLE_MS` | 20 ms | Shell cadence |
| `DISPLAY_FILTER_TAU_MS` | 200 ms | [75] |
| `DISPLAY_FILTER_RESET_MS` | 1000 ms | [75] |
| `DISPLAY_ACCEL_TOLERANCE_G` | 0.4 g | [75], same tolerance as gravity |
| `DISPLAY_NEAR_ZERO_G` | 0.1 g | [75] |
| `DISPLAY_FLAT_DEG` | 80 degrees | [75] |
| `DISPLAY_FLAT_MS` | 1000 ms | [75] |
| `DISPLAY_SWING_DEG` | +20 degrees | [75] |
| `DISPLAY_SWING_MS` | 300 ms | [75] |
| `DISPLAY_MAX_TILT_DEG` | 80 degrees | [75] |
| `DISPLAY_OVERHEAD_ENTER_DEG` | -40 degrees | [75] |
| `DISPLAY_OVERHEAD_EXIT_DEG` | -15 degrees | [75] |
| `DISPLAY_HYSTERESIS_GAP_DEG` | 45 degrees | [75] |
| `DISPLAY_SETTLE_MS` | 40 ms | [75] |
| `DISPLAY_AFTER_FLAT_MS` | 500 ms | [75] |
| `DISPLAY_AFTER_ACCEL_MS` | 500 ms | [75] |
| `DISPLAY_AFTER_SWING_MS` | 300 ms | [75] |
| `DISPLAY_AFTER_TOUCH_MS` | 500 ms | [75] |
| `DISPLAY_TILT_MIN_DEG` | -25 degrees | [75] |
| `DISPLAY_TILT_DEFAULT_MAX_DEG` | 70 degrees | Maintainer: default quarter |
| `DISPLAY_TILT_ADJACENT_MAX_DEG` | 65 degrees | Maintainer: adjacent quarters |
| `DISPLAY_TILT_OPPOSITE_MAX_DEG` | 60 degrees | Maintainer: opposite quarter |

An adjacent quarter needs more than `(90 + gap) / 2` degrees from the
committed axis. A prediction keeps its timer while unchanged.
Flat, swing, acceleration and touch each postpone commitment.

`display_hold_quarter(display_quarter_now())` locks rotation and stops
sensor reads. Release resets the filter and prediction; the committed
quarter changes only after the normal gates pass.

The console IMU reply gives `sample_ms` and `commit_ms`. The latter includes
a filter-flush allowance, cooldown and settle time for a steady unit sample.
A continuing touch or rotation hold can prevent commitment indefinitely.

## Replay

The guided orientation round records FIFO accel and gyro counts with
microsecond timestamps in the shared `labdump` transcript. Each ready gate
records samples while the person turns; a tap starts its timed reading.
Step and gate markers carry elapsed times.
Its capture commands are in the round's README.

```sh
python launcher/tools/input/orientation_replay.py round.log --model both --initial-quarter 1 --cc gcc
```

The C probe links the real display and gravity modules. `accel` uses the
judge's low-pass filter; `fused` feeds unit `gravity_down()` after every FIFO
sample, so its magnitude gate sees only the judge filter's vector length.

Both models retain the judge filter and timing gates. Fusion updates at
FIFO cadence; judge runs use 20 ms and 100 ms deadlines, taking the first
available sample after each deadline.

The scorer reads the unfiltered screen-axis acceleration. A turn begins when
its x/y angle crosses the hysteresis gap edge away from the committed quarter.
It lands when the new quarter is held inside its tilt limits and the judge
commits it. Latency starts at that raw edge crossing, independent of prompts.
A commit without a matching turn, or during a flat or keep step, is unwanted.
The segment targets validate the recorded script only.

<!-- generated: orientation-results check: python launcher/tools/input/orientation_replay.py --results --check --cc gcc -->
| Model | Cadence | Wanted turns | Unwanted switches | Median latency | Max latency |
|---|---:|---:|---:|---:|---:|
| accel | 20 ms | 4 | 0 | 338 ms | 1285 ms |
| accel | 100 ms | 4 | 0 | 451 ms | 1425 ms |
| fused | 20 ms | 4 | 0 | 360 ms | 1285 ms |
| fused | 100 ms | 4 | 0 | 501 ms | 1425 ms |
<!-- /generated: orientation-results -->

Accel and fused land the same turns without unwanted switches on this
capture. The shell uses low-passed accel at `DISPLAY_SAMPLE_MS`. FIFO gaps
remain visible as timestamp gaps; replay does not reconstruct missing samples.
Touch is absent from replay.

Portrait crosses the raw gap edge at 23.89 s and commits at 25.17 s.
The filtered angle hysteresis holds for 1.24 s; settle is last at 45 ms.
Raw angles briefly cross, then linger at 50 to 60 degrees.

`--gates` reports the last gate holding each commit and its hold time. It also
shows the longest hold between raw edge crossing and commitment.

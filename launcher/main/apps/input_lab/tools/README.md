# Orientation capture and replay

BOOT selects the orientation page. Hold the default reading pose and tap to
start recording. Each ready gate records the turn; a fresh tap starts the
timed reading. The script targets come from `../orientation_probe.h`.

| Steps | Target |
|---|---|
| Reading poses | Start at `DISPLAY_DEFAULT_QUARTER`, advance modulo `DISPLAY_QUARTER_COUNT`, finish at the default |
| Flat | Face up, no quarter target |
| Pickup, walk, shake, overhead | Keep the default quarter |
| Done | No target, collect the transcript |

`IMU_RECORD_ORIENTATION_CAPACITY` in `../imu_record.h` reserves PSRAM for timed
steps and ready dwell. A full transcript is reported when that budget is
exceeded; restart the round for a complete dump.

```sh
autana labdump > orientation.log
python launcher/main/apps/input_lab/tools/orientation_replay.py orientation.log --model both --initial-quarter 1 --cc gcc --gates
```

`--initial-quarter` is the committed quarter at recording start.
The C probe validates segment targets against `orientation_probe_target()`.
Step and gate markers retain elapsed microsecond timestamps.

| Replay choice | Behavior |
|---|---|
| `accel` | Offset-calibrated counts through the judge's low-pass filter |
| `fused` | Unit `gravity_down()` updated at FIFO cadence, then the same judge filter and gates |
| Judge cadence | First available sample after each fine or coarse deadline |
| Wanted turn | Raw acceleration crosses the C candidate rule's gap edge, satisfies tilt and settle, then commits |
| Unwanted switch | Commit without a matching turn, or during a flat or keep step |
| Latency | Raw gap-edge crossing to commitment, independent of prompts |
| `--gates` | Last blocking gate and longest hold for each commit |
| Missing input | Timestamp gaps remain visible; touch is absent |

The scorer and judge share `display_candidate()` in
`display/display_judge.h`. Segment targets validate the script only.

The retained transcript is `data/orientation.log.gz`; provenance is in
`data/orientation.json`. Replay reads gzip directly. Generate or check the
results in the [app README](../README.md):

```sh
python launcher/main/apps/input_lab/tools/orientation_replay.py --results --cc gcc
python launcher/main/apps/input_lab/tools/orientation_replay.py --results --check --cc gcc
```
`display/display_judge.h` and the [engine judge](../../../../../docs/input/Orientation.md) describe the filter and event gates.

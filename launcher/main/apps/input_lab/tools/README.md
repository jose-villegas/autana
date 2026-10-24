# Orientation capture

BOOT selects the orientation page. Hold landscape at 45 degrees and tap to
start recording. Each step opens a ready box: turn toward the named pose while
it records, then tap to start its timed reading. Follow the arrow and live
rotation and tilt angles. Read the full turn back to landscape, then put the
screen face-up, pick up, walk, shake and read overhead while keeping landscape.

Reading targets start at `DISPLAY_DEFAULT_QUARTER`, advance modulo
`DISPLAY_QUARTER_COUNT`, and finish at the default. Pickup, walking, shaking
and overhead targets keep the default; flat and completion have no target.

The HUD names the committed quarter in a fixed reading transform. White flashes
mark steps; cyan marks completion. The shared recorder retains FIFO accel
and gyro counts with microsecond timestamps.

`IMU_RECORD_ORIENTATION_CAPACITY` in `../imu_record.h` reserves space for
timed steps and ready dwell in PSRAM. The HUD reports a full transcript when
the ready gates exceed that capacity; restart the round for a complete dump.
Step and ready-gate markers carry elapsed microsecond timestamps.

```sh
autana labdump > orientation.log
python launcher/tools/input/orientation_replay.py orientation.log --model both --initial-quarter 1 --cc gcc --gates
```

`--initial-quarter` is the quarter actually committed at recording start,
not the first requested pose. The replay probe uses `orientation_probe_target()`
to validate each segment annotation against the capture script.

The models and segment metrics are described in [Orientation.md](../../../../../docs/input/Orientation.md).

The committed orientation transcript is `data/orientation.log.gz`, with
provenance in `data/orientation.json`. Replay reads gzip directly. Generate or check the results in the orientation guide:

```sh
python launcher/tools/input/orientation_replay.py --results --cc gcc
python launcher/tools/input/orientation_replay.py --results --check --cc gcc
```

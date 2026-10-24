# Orientation capture

BOOT selects the orientation page. Hold landscape at 45 degrees; tap to start.
Read a full turn in quarter-turn steps back to landscape. Put the screen
face-up, then pick up, walk, shake and read overhead while keeping landscape.

Reading targets start at `DISPLAY_DEFAULT_QUARTER`, advance modulo
`DISPLAY_QUARTER_COUNT`, and finish at the default. Pickup, walking, shaking
and overhead targets keep the default; flat and completion have no target.

The HUD names the committed quarter in a fixed reading transform. White flashes
mark steps; cyan marks completion. The shared recorder retains FIFO accel
and gyro counts with microsecond timestamps.

```sh
autana labdump > orientation.log
python launcher/tools/input/orientation_replay.py orientation.log --model both --initial-quarter 1 --cc gcc
```

`--initial-quarter` is the quarter actually committed at recording start,
not the first requested pose. The replay probe uses `orientation_probe_target()`
to validate each segment annotation against the capture script.

The models and segment metrics are described in [Orientation.md](../../../../../docs/input/Orientation.md).

# Orientation capture

BOOT selects the orientation page; tap to start. The prompts guide reading
at 45 degrees through each quarter, putting the screen face-up, picking it
up, walking, shaking and reading overhead. Keep quarter 3 after its prompt.

The HUD shows the committed quarter in a fixed reading transform. White flashes
mark steps; cyan marks completion. The shared recorder retains FIFO accel
and gyro counts with microsecond timestamps.

```sh
autana labdump > orientation.log
python launcher/tools/input/orientation_replay.py orientation.log --model both --initial-quarter 1 --cc gcc
```

Pass the committed quarter from the start of the round. The models and
segment metrics are described in [Orientation.md](../../../../../docs/input/Orientation.md).

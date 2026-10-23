# Motion performance

`run_sand_motion_perf_suite` measures the per-frame gravity and shake path,
including the jostle mapping. Run it on a diagnostics image:

```sh
autana suite run_sand_motion_perf_suite --flash --perf-scope
```

The replay concatenates STILL, HARD and STILL excerpts from the retained
`shake-mixed.log.gz` capture. Its generated header records the regeneration
command. Samples use calibrated screen-axis counts and `SAND_STEP_MS` intervals.

One pass has zero rotation; another uses constant screen-axis rotation from
the suite's `MOTION_ROTATION_*_DPS` constants. These rates exercise gyro prediction;
the capture contains acceleration only.

`MOTIONBENCH` rows report gravity alone, shake/level/jostle alone, and their
combined step. Sensor reads and tilt controls are outside the measurement.
The shake-only row receives linear acceleration computed outside its timer.

Each frame uses math_perf's warm and cold repetition counts and shared cache
invalidation. Every repetition starts from the same filter state; only the
untimed reference step advances the sequence.

The reported values average the per-frame cycle medians across the replay.
Cold invalidates flash instructions and rodata before every timed frame.
The input buffer stays in internal RAM and is freed after each pass.

Microseconds derive from the configured CPU frequency. Frame percentages use
`SAND_STEP_HZ`, including for the whole-step row. Timing excludes cache
invalidation, state copies, reference steps, statistics and scheduler yields.

The host runs the replay's activity and settling assertions; hardware timing
is skipped there and under QEMU. Run the portable check explicitly:

```sh
launcher/test/build/host_tests --run run_sand_motion_perf_suite
```

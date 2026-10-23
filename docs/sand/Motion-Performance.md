# Motion performance

`run_sand_motion_perf_suite` measures the per-frame gravity and shake path,
including the jostle mapping. `--perf-scope` builds the diagnostics image from
the engine and app `scope_perf.cmake` manifests:

```sh
autana suite run_sand_motion_perf_suite --flash --perf-scope
```

The suite builds STILL, HARD and STILL segments at `SAND_STEP_MS` intervals.
STILL has one g down and no linear acceleration. HARD adds sinusoidal linear
acceleration at the named hand-shake frequency and amplitude in the suite.

One pass has zero rotation; another uses constant screen-axis rotation from
the suite's `MOTION_ROTATION_*_DPS` constants to exercise gyro prediction.

`MOTIONBENCH` rows report gravity alone, shake/level/jostle alone, and their
combined step. Sensor reads and tilt controls are outside the measurement.
The shake-only row receives linear acceleration computed outside its timer.

Each frame uses the shared cycle sampler's repetition counts and cache invalidation. Every repetition starts from the same filter state; only the
untimed reference step advances the sequence.

The reported values average the per-frame cycle medians across the replay.
Cold invalidates flash instructions and rodata before every timed frame.
The input buffer stays in internal RAM and is freed after each pass.

Microseconds derive from the configured CPU frequency. Frame percentages use
`SAND_STEP_HZ`, including for the whole-step row. Timing excludes cache
invalidation, state copies, reference steps, statistics and scheduler yields.

The host checks zero jostle in the initial and settled final STILL segments,
plus onset and full jostle during HARD, with zero rotation. Hardware timing is
skipped there and under QEMU. Run the portable check explicitly:

```sh
launcher/test/build/host_tests --run run_sand_motion_perf_suite
```

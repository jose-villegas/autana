# Testing Sand

Everything about verifying the sand app on real hardware that is specific
to this app, split out of [`../Testing-Guide.md`](../Testing-Guide.md)
(read that first for the host/device split, RUNSUITE, and the general
practice). This page is the sand-only half: the frame-budget capture, the
free-heap precondition it depends on, and how to read what it prints.

---

## Everyday loop: RUNSUITE, not a capture

A diag build (`CONFIG_LAUNCHER_SELFTEST` on, `AUTORUN` off) listens on the
USB serial console. Sending

```
RUNSUITE run_sand_perf_suite
```

runs the sand frame-budget suite alone, on the board already flashed, with
no rebuild and no reflash. Any other sand suite works the same way -
`run_sand_materials_suite`, `run_sand_combustion_suite`, and so on; see
`docs/Testing-Guide.md`'s suite table for the full list. This is the loop
while working on a material or a perf change: RUNSUITE the suite for the
area touched, and reserve a full capture for a merge decision.

## Capturing a frame-budget report

`report_performance.sh` (`launcher/main/apps/sand/tools/`) builds and
flashes the diagnostics variant, captures the frame-budget suite's run and
writes a markdown table; `compare_reports.py` diffs two such reports, and
`report_performance.sh --baseline <report>.md`/`--no-restore` run that
comparison as part of a capture.

Three rules keep a reading honest:

- **Only within-capture comparisons are trustworthy.** Two separately
  linked images of identical source can disagree by several percent
  purely from where things land in flash. Never read a delta between two
  different captures' numbers for the same row as real without checking
  that the pair's own noise floor (`compare_reports.py` derives one from
  the two control rows in the pair actually being compared) is smaller
  than the delta.
- **Check the control rows before reading anything else.** Every capture
  carries at least one movement-free control row alongside the timed
  scenes. If a change that should only touch one material moves a
  control row too, the capture's flash layout shifted, and every other
  delta in that capture needs to be read against that shift rather than
  treated as real on its own.
- **Measure landscape first.** Landscape is the shipping orientation -
  gravity moves within a fixed grid, so a portrait-tuned scene measures
  the wrong thing. A portrait-only frame-budget row has hidden real costs
  before: rotated UI work, and sand rows running along gravity rather
  than across it.

### Free heap is no longer the precondition it was

Every frame-budget scene mallocs its grid (one grid is ~41 KB), so on a
board where that allocation could fail, a short heap produced a
clean-looking, worthless report: the suite still ran, still printed
completion, with no timings in it at all.

On this board that failure mode is gone. The framebuffer lives in PSRAM,
not internal DRAM, so it no longer competes with the sand grid for
internal-heap contiguity, and the measured internal heap free after
`gfx_init()` is 184,171 bytes (172,147 once the shell is ready) - more
than four sand grids' worth of contiguous headroom. Memory is not a
constraint for this suite any more. Still worth grepping a raw capture
for `free heap after framebuffer` if a report ever comes back with zero
timings, since a heap short for an unrelated reason (a leak in an earlier
suite, a scoped build's own footprint) would look the same.

## Perf sanity, not just logging

A perf test should assert that work actually happened - bytes sent
greater than zero, a frame time not impossibly fast - not only log a
number. A logging-only band-mode test once passed while the mode
rendered nothing at all. Log the measurement **before** asserting on it,
so a failing budget still prints what it measured instead of losing the
number to the same assert that failed.

## Scoping a build for a capture

The perf scope (`CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF`) compiles only
`suite_sand_perf.c` plus the scene builders and fixtures it calls -
`suite_sand_scenes.c` and `suite_sand_common.c` - instead of every suite.
On a board where the framebuffer shared internal DRAM with `.bss`, this
once bought back static-RAM headroom a capture needed to run at all. On
this board the framebuffer lives in PSRAM, so scoping no longer buys
memory - it only buys run time (3 suites instead of the full run) and
changes the image's layout in the 32 KB instruction cache. That second
effect means **a scoped capture's numbers compare only with other scoped
captures**, never with an unscoped run - same reasoning as the
within-capture-only rule above, one layer up.

```sh
bash launcher/main/apps/sand/tools/report_performance.sh --perf-scope
```

Use scoping for perf captures only, not for a merge decision - the full
self-test is what gates a merge, and it runs unscoped by construction.

## The thirteen frame-budget tests

`suite_sand_perf.c`, `#ifdef DEVICE_BUILD` only, run against the real
184x224 grid. In the current full self-test (S3, full scope, landscape,
2026-09-14) twelve of these thirteen fail - each pegged to a budget
measured on the previous board and not yet re-measured on this one. That
is the expected, tracked state: re-peg each budget from a fresh landscape
capture on this board rather than reading a fail as a regression. Read
each test's own comment in `suite_sand_perf.c` for what its scene measures
and why its budget sits where it does.

## Related

- [`../Testing-Guide.md`](../Testing-Guide.md) - the host/device split,
  RUNSUITE, the two device-only traps, and the suite-to-area table.
- [`Architecture.md`](Architecture.md) - the app's own shape: the grid,
  the material tables, the step pipeline.

# Testing Sand

Everything about verifying the sand app on real hardware that is specific
to this app, split out of [`../Testing-Guide.md`](../Testing-Guide.md)
(read that first for the host/device split, RUNSUITE, and the general
practice). This page is the sand-only half: the frame-budget capture, the
free-heap precondition it depends on, how to read what it prints, and the
chunk layout sweep that picks the app's two-core geometry.

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

A timed fixture opens `board_bookkeeping_open()` (`suite_sand_common.c`) on
its grid, which gives it what `alloc_grid_bookkeeping()` (`app_sand.c`) gives
a shipped board: dirty rows and columns, step stamps, lane scratch, and block
state where the scene brought none. Without the last two no pass is ever
ready to split, so the row times one core however many it asked for. What a
scene is made of stays the fixture's own business. A frame-budget board is
held to that by `test_a_frame_budget_board_really_reaches_the_split_path`,
which fails unless a busy full-size step dispatches at least one split pass.

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
internal-heap contiguity. It is not roomy either: the measured internal
heap free after `gfx_init()` is 130,635 bytes (117,219 once the shell is
ready), and its largest block is 51,200 - one 41,216-byte sand grid fits,
a second contiguous one does not. Still worth grepping a raw capture
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

## The chunk layout sweep

The two-core chunk geometry is measured rather than reasoned out: the
`(side_x, side_y)` pair each quality grid cuts a pass into, per travel
class, and the grid size below which every pass stays on one core. The
shipped answers are in [`Sand-Simulation.md`](Sand-Simulation.md#the-chunks);
this is how a round of measuring produces them.

It runs in three places; each says more than the one before it, and costs
more to run.

**Host pre-filter.** `launcher/main/apps/sand/tools/report_chunk_layout.sh`,
about 15 s. It walks the real split path on one lane, charges each chunk the
cells its passes dispatched, and ranks how evenly a layout divides a board's
work. That produces a shortlist of side pairs per quality and nothing else:
no time of any kind.

**QEMU.** One instance per quality against one perf-scope image, all from
the sweep driver in `.dev/scripts/`:

```sh
.dev/scripts/qemu-sweep.sh --instances 5
```

It prices the chunking itself, through the one-thread arm. The two-lane arm
is readable there only for its abort count, because an instruction count
sums both cores - see
[`../Testing-Guide.md`](../Testing-Guide.md#qemu-the-device-image-with-no-board)
for why, and for what several instances at once cost each other.

**The board.** The only stage that says whether the second core wins. Five
on-request suites, one per quality, live in the perf-scope diagnostics
image and run by name:

```
RUNSUITE run_chunk_sweep_ultra_suite
RUNSUITE run_chunk_sweep_high_suite
RUNSUITE run_chunk_sweep_normal_suite
RUNSUITE run_chunk_sweep_low_suite
RUNSUITE run_chunk_sweep_very_low_suite
```

On request means no autorun pays for them: a full self-test never runs a
sweep it was not asked for.

### The four arms

Every cell is measured four ways, named in the line's `arm=` field:

- `serial` - the plain one-core walk, which is what a split has to beat.
- `serial-hashed` - that same walk drawing the split's per-cell hash, so
  the hash is priced apart from the chunking that needs it.
- `solo` - the chunk order walked by one thread, which is the chunking's
  own cost with no second core in it.
- `split` - the real two-lane step.

### What it prints

One `CHUNK_SWEEP` line per cell, with the fields `quality=`, `grid=`,
`side=`, `scene=`, `orient=`, `arm=`, `us_per_step=`, `aborts=`, `chunks=`,
`sweep_us=`, `liquid_us=`, `gas_us=`, `react_us=` and `other_us=`. The last
five break one step down by pass, so a layout that helps the liquid passes
and hurts the gas pass is visible instead of averaged into one number.

Each quality opens with a `CHUNK_SWEEP_FLOOR` line - `quality=`,
`serial_us=`, `split_us=`, `settle_steps=` - a settled pile stepped serial
against split on the shipped side, which is what involving the second core
costs before any work is handed to it. `settle_steps` is how many steps the
pile needed to stop changing; at the cap it never came to rest, and the two
numbers beside it are a transient rather than a floor. A quality closes
with `CHUNK_SWEEP_COMPLETE`.

Under `--icount` a `us_per_step` field times 1000 is instructions per step.
On the board it is microseconds.

### The lists, and what a round costs

Each quality's side list carries both cuts that quality ships and the
square cut they replaced, so a round stays comparable with the one before
it; `test_the_sweep_measures_both_cuts_every_quality_ships` fails if a
shipped cut is missing from the list it is ranked against.

The scene list ends in a gas pair - an open block still climbing through
the measured window, and a sealed box whose gas has packed against a wall -
so the gas walk and the gas spread are each ranked on work they really do,
which `test_the_sweeps_gas_scenes_put_work_in_both_gas_passes` checks on a
host.

A quality's cell count is its side list times the scene list times two
orientations times four arms; `sweep_qualities`, `sweep_scenes` and
`sweep_orients` in `suite_sand_perf.c` are those lists, and reading them
beats any count written here. As an order of magnitude, a quality carrying
five sides is a few hundred cells, at roughly 0.8 s of board time each - so
one quality is minutes on the board, and longer under emulation.

## The frame-budget tests

`suite_sand_perf.c`, `#ifdef DEVICE_BUILD` only, runs 28 frame-budget
tests against the real 184x224 grid. Most of their budgets are still
pegged to numbers measured on the previous board and fail here until
re-measured and re-pegged on this one; that is the expected, tracked
state, not a regression. Read each test's own comment in
`suite_sand_perf.c` for what its scene measures and why its budget sits
where it does.

## Related

- [`../Testing-Guide.md`](../Testing-Guide.md) - the host/device split,
  RUNSUITE, the two device-only traps, and the suite-to-area table.
- [`Architecture.md`](Architecture.md) - the app's own shape: the grid,
  the material tables, the step pipeline.

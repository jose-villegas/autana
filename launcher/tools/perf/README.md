# Performance comparison

`perf_compare.sh A B` compares revisions using independent, random positive
layout seeds on each side. `A A` measures the same source with two distinct
seed sets. The wrapper uses `revision_worktree.sh` for detached revision
checkouts and restores `origin/main`'s release image afterwards. Directory
arguments use existing project trees. `--no-restore` omits the restore flash.

Each `--suite NAME TESTS TABLE` uses the layout pilot's triple: a registered
suite name, comma-separated test patterns (`-` for all), and a table command
(`-` to read timing rows directly from the capture). Commands run from the
current repository; `@CAPTURE@`, `@TABLE@` and `@PROJECT@` are replaced with file paths.
The table command must read suite budgets from the revision being compared
when budgets are needed; comparison itself reads measured timing values.

```sh
launcher/tools/perf/perf_compare.sh A B -o comparison --rng-seed 42 \
  --suite run_sponza_perf_suite flythrough -

launcher/tools/perf/perf_compare.sh A A -o calibration \
  --suite run_sand_perf_suite frame_budget \
  'python3 launcher/main/apps/sand/tools/report_performance.py @CAPTURE@ @TABLE@ --source @PROJECT@/launcher/main/apps/sand/tests/suite_sand_perf.c'
```

For a report requiring budgets, add `--source PATH` to its table command.
Both the pilot and comparison import capture acquisition, table parsing,
wall timing and variance estimates from `layout_measure.py`. The comparison
uses the existing seeded build mechanism and its sdkconfig pad geometry;
it does not implement padding itself.

## Measurement plan

`--rng-seed` replays the random seed and interleaving choices. Every A/B
pair randomises which side flashes first, and seeds are distinct across
both sides. `plan.json` records the order, seed, runs, selected suites and
capture failures. Each seed flashes once, then runs its selected tests
repeatedly. The first pass uses two runs per seed and the smallest seed
count whose exact two-sided permutation test can reach the per-look alpha.
`comparison.json` and `summary.md` state the first pass and cap actually used.

`--max-seeds` caps seeds per side. The default bounds capture cost; increase
it when an inconclusive result justifies more measurement. Planned looks
start at the first-pass count, double towards the cap and end at the cap.
The nominal alpha is divided across all planned looks to account for
repeated decisions. Holm adjusts difference and equivalence tests separately
across the row family, including rows decided on earlier looks.

Measured build/flash/boot and run wall times, sigma_run and sigma_flash
feed the pilot's Kalibera-Jones R* and required K calculation. K selects the
next planned look, bounded by the cap; a flash uses the largest recommended
R among rows selected for that pass. When estimated layout variance is zero,
run count follows measured flash/run cost (and stays at least two).
Inconclusive rows alone receive extra seeds. Capture result lines map rows
to the tests that print them; an unmapped row uses the original suite filter.

`--timeout` sets the capture deadline; `--wait` sets lock queue time. The
foreground command deadline allows that wait and all requested runs. Each
flash checks the logged boot id against its project tree's seeded build id.
Later suites on that flash request that same build id. A complete capture
that exits 1 but has timing rows is kept. An incomplete capture, missing rows,
wrong build or other capture failure is recorded; two consecutive failures
stop the comparison. Failed attempts consume the cap. Summaries state
successful seeds and their build ids.

## Reading the result

A seed's mean of runs is one independent observation. The table displays
arithmetic means and medians of seed means, B/A, delta time and a Welch
interval on log seed means. The interval describes the geometric ratio;
B/A is the ratio of arithmetic means. Existing instruction-counter lines
are paired through capture test ownership and exact scene names; only
unambiguous, non-overflowing pairs produce delta-insn.

`--threshold PCT` states the equivalence margin. `--alpha` states the nominal
family error rate. **No change** requires TOST equivalence within that margin,
with Holm adjustment. **Regressed** or **improved** requires a Holm-adjusted
Welch difference from one and agreement from an exact permutation test on
the same log seed means (Monte Carlo for large partitions). A permutation
p-value is `n/a` if the seed count cannot reach the per-look alpha. **Inconclusive**
covers everything else, including insufficient or zero timings and rows
undecided at the cap. A/A calibration allows inconclusive noisy rows; lack
of significance alone does not establish equivalence.

There are no row allowlists or fixed noise floors. Row statistics, estimates,
the plan and capture records are preserved beside the summary. Host tests
inject an autana-compatible runner for acquisition and exercise the shell
wrapper with `--autana COMMAND`; this option does not require hardware.

```sh
python -m unittest discover -s launcher/tools/perf/tests
```

See [Layout-Noise.md](../../../docs/tools/Layout-Noise.md) for the seeded
build and pilot variance definitions.

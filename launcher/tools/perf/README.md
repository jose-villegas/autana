# Performance comparison

`perf_compare.sh A B` compares revisions using independent, random positive
layout seeds on each side. `A A` calibrates the same source with two distinct
seed sets. Arguments accept revisions or existing project directories.
The wrapper restores `origin/main`'s release image afterwards;
`--no-restore` omits that flash.

Each `--suite NAME TESTS TABLE` takes a registered suite name, comma-separated
test patterns (`-` for all), and a table command (`-` reads the capture's
`<row> both cores: mean <N>us` lines). Pattern width and count limits come
from each project's `launcher/test/suites.h`; a pattern over the width is
refused before any flash, and patterns over the count run as several requests
on the same flash. Each test must match patterns in one request only; a row
measured by two requests stops the comparison. Commands run from the current repository: `@CAPTURE@` and
`@TABLE@` become this flash's capture and table files, `@PROJECT@` the revision's
project tree. A table command requiring budgets must read them from that tree,
for example with `--source @PROJECT@/path/to/suite.c`.

```sh
launcher/tools/perf/perf_compare.sh A B -o comparison --rng-seed 42 \
  --suite run_sponza_perf_suite flythrough -

launcher/tools/perf/perf_compare.sh A A -o calibration \
  --suite run_example_perf_suite frame_budget \
  'python3 path/to/report_performance.py @CAPTURE@ @TABLE@ --source @PROJECT@/path/to/suite.c'
```

`--perf-scope` is forced for every seeded flash.
Captures, status checks and restore use `autana` on PATH; `--autana COMMAND`
overrides all three for a host replay.

## Measurement plan

The default RNG seed is a fresh draw. `--rng-seed` replays a recorded draw;
`plan.json`, `summary.md` and `comparison.json` record it. Every A/B pair
randomises which side flashes first, with distinct seeds across both sides.
`plan.json` records each flash's seed, side, runs, suites and any capture error.

The first pass takes the seed count needed for exact permutation resolution
at nominal alpha, repeats a subset to estimate run variance, and captures
the remaining seeds once. The summary states actual first-pass size, runs
per look, cap and run clamp. Run variance is pooled from repeated seeds;
every complete seed contributes a mean and layout estimate.

```mermaid
flowchart TD
    First[First pass] --> Fail{Two consecutive failures?}
    Fail -->|No| Decide[Decide rows]
    Decide --> Active{Inconclusive rows with data on both sides?}
    Active -->|No| Summary[Write summary]
    Active -->|Yes| Cap{At seed cap?}
    Cap -->|Yes| Summary
    Cap -->|No| Estimate[Estimate K and bounded R from variance and costs]
    Estimate --> Next[First planned look at or above K and past this one, else the cap]
    Next --> Capture[Capture active rows]
    Capture --> Fail
    Fail -->|Yes| Incomplete[Write incomplete summary]
```

Planned looks double towards `--max-seeds` and end at the cap. Nominal alpha
is divided across planned looks, then split equally between Welch difference
and TOST equivalence families. Each family uses Holm adjustment across rows,
including rows decided earlier. Permutation is an AND cross-check at nominal
alpha. Its Monte Carlo resample count keeps the add-one floor well below that
alpha; settings exceeding the supported resample budget are refused before
measurement.

Measured build/flash/boot and run costs, sigma_run and sigma_flash feed
Kalibera-Jones R and required K. K uses the per-look Holm share and one-sided
TOST alpha. R is clamped between 1 and `MAX_RUNS` (`perf_compare.py`), also
when the estimated layout variance is zero; the summary states the clamp.
A later flash uses the largest R recommendation among active rows.
For a filtered first pass, extra seeds re-run the subset of the user's own
patterns matching active tests. For an unfiltered first pass, capture test
ownership maps active rows to the shortest substrings unique among the tests
the unfiltered pass reported (PASS, FAIL and IGNORE result lines). Missing inventory,
unknown owners, or tests without a bounded unique substring fall back to the
user's filter.

Each suite's deadline is `--timeout` per requested run plus `--wait`.
Table commands also run with a deadline of `--timeout`. Status logs
bracket each flash; the boot id must match its project's seeded build id.
Later suites request that build id. Complete captures exiting 1 are kept;
other exit codes, incomplete captures, wrong builds and table errors are
failures. Two consecutive failures stop measurement, and so does a suite
whose first flash on each side gives no timing rows; that error names the
suite and its table. A stop writes the summary so far, with decisions and
errors, and marks it incomplete, as does any row not measured. A success
resets the failure count. Failed attempts consume the cap.

## Reading the result

Each complete seed mean is one independent observation. A row missing from
any run of a seed excludes that seed for that row; expected rows remain
visible even when a later flash omits them. The table shows arithmetic means,
medians of seed means, B/A, delta time and a Welch interval on log seed means.
The interval describes a geometric ratio, whereas B/A uses arithmetic means.
Instruction deltas use complete, unambiguous, non-overflowing counter pairs
from the look that decided the row.

No change means Holm-adjusted TOST equivalence within +/-threshold; regressed
or improved means a significant Welch difference with permutation agreement
that is not shown equivalent, so a move smaller than the threshold can still
be regressed.

A real move inside the threshold can be reported as **no change**, so to see
a small gain as **improved**, pass a `--threshold` below the gain you are
chasing (for example `--threshold 0.1` for a 0.3% target). A smaller threshold
needs more seeds per side; raise `--max-seeds` with it, or expect inconclusive
rows at the cap.

Both arithmetic and log-difference signs must agree on direction. **Added**
means B only and **removed** A only when every attempt on the other side
succeeded and its captures ran the row's owning test without printing the
row. An IGNORE result does not establish measured absence.
**Not measured** means missing or zero timings or no complete seed.
These rows receive no extra seeds.
**Inconclusive** rows with data on both sides alone receive more measurements,
and remain inconclusive when the cap is reached. Permutation n/a means there
is too little positive timing data to compute it. A/A calibration allows
inconclusive noisy rows; lack of significance does not establish equivalence.

Statistics, variance estimates, seed/build identities and capture records
are preserved beside the summary. Host tests use an injected runner:

```sh
python -m unittest discover -s launcher/tools/perf/tests
```

`phase_report.py` writes the phase tables for a reporter that turns one
device capture into Markdown: each phase's average per run, Total as fps,
then each run's min/max/avg/median/p95 per phase.

See [Layout-Noise.md](../../../docs/tools/Layout-Noise.md) for seeded builds
and pilot variance definitions.

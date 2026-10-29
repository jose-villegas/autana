# Performance comparison

`perf_compare.sh A B` compares revisions using independent, random positive
layout seeds on each side. `A A` calibrates the same source with two distinct
seed sets. Arguments accept revisions or existing project directories.
The wrapper restores `origin/main`'s release image afterwards;
`--no-restore` omits that flash.

Capture comparisons require one USB serial throughout the input set.
A mixed set exits non-zero before comparison and lists each board's short
id, full serial and captures. A capture without board provenance is unknown:
unknown captures can be compared together, but cannot be mixed with a known
board. The shared identity check in `capture.py` reads board fields in saved
measurement records and the device capture index. Raw captures need matching
device index entries; seeded measurement and sweep records retain the serial.
A held comparison job routes every capture through the same board.

`autana status` shows the job name, estimated finish and queued starts.
Estimates use recorded flash and capture durations, with the configured capture
deadline and `DEFAULT_FLASH_SECONDS` in `perf_job.py` as fallbacks.
The comparison estimate covers its first scheduled pass: seeds per side,
calibration repeats, requests per capture and the restore flash when enabled.
Adaptive extra seeds can run longer. The sweep estimate uses its planned
flashes, rounds, knob points and inventory captures, plus the optional restore.

```mermaid
sequenceDiagram
    participant S as Shell wrapper
    participant L as autana lock
    participant P as perf Python
    alt No inherited token
        S->>L: take with owner pid and estimate
        L-->>S: token after queue wait
    else Borrowed token
        Note over S,L: Outer hold remains owned by caller
    end
    S->>P: Seed flashes and captures with token
    alt Success
        P-->>S: Results
    else Failure or Ctrl+C during capture
        P-->>S: Stop acquisition
    end
    opt Restore enabled after acquisition
        S->>P: Restore release image with token
        P-->>S: Restore finished
    end
    alt Token taken by wrapper
        S->>L: release token
    else Borrowed token
        Note over S,L: Caller releases outer hold
    end
    Note over S,L: Interrupted queued take skips restore and release
```

Each `--suite NAME TESTS TABLE` takes a registered suite name, comma-separated
test patterns (`-` for all), and a table command (`-` reads the capture's
`<row> both cores: mean <N>us`, `<row> one core: mean <N>us`,
`<row> mean <N>us` lines, or a `frame_profile:` row). Pattern width and count limits come
from each project's `launcher/test/suites.h`; a pattern over the width is
refused before any flash, and patterns over the count run as several requests
on the same flash. Each test must match patterns in one request only; a row
measured by two requests stops the comparison. Commands run from the current repository: `@CAPTURE@` and
`@TABLE@` become this flash's capture and table files, `@PROJECT@` the revision's
project tree. A table command requiring budgets must read them from that tree,
for example with `--source @PROJECT@/path/to/suite.c`.

```sh
launcher/tools/perf/perf_compare.sh A B -o comparison --rng-seed 42 \
  --suite SUITE TESTS -

launcher/tools/perf/perf_compare.sh A A -o calibration \
  --suite run_example_perf_suite frame_budget \
  'python3 path/to/report_performance.py @CAPTURE@ @TABLE@ --source @PROJECT@/path/to/suite.c'
```

`--send-a LINE` and `--send-b LINE` send console setup before every capture.
Each side's sequence should be idempotent: stop voices before starting a loop.
Use the same revision on both sides to measure runtime setup alone:

```sh
launcher/tools/perf/perf_compare.sh HEAD HEAD -o sound-comparison \
  --suite run_sponza_perf_suite flythrough - \
  --send-b 'sound stop all' --send-b 'sound play tap loop'
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
is divided across planned looks, then split equally between Welch difference<sup>[[37]](../../../docs/Citations.md#welch-1947)</sup>
and TOST equivalence<sup>[[38]](../../../docs/Citations.md#schuirmann-1987)</sup> families. Each family uses Holm adjustment<sup>[[39]](../../../docs/Citations.md#holm-1979)</sup> across rows,
including rows decided earlier. Permutation is an AND cross-check at nominal
alpha. Its Monte Carlo resample count keeps the add-one floor<sup>[[40]](../../../docs/Citations.md#phipson-2010)</sup> well below that
alpha; settings exceeding the supported resample budget are refused before
measurement.

Measured build/flash/boot and run costs, sigma_run and sigma_flash feed
Kalibera-Jones<sup>[[41]](../../../docs/Citations.md#kalibera-2013)</sup> R and required K. K uses the per-look Holm share and one-sided
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

## Sweep

`perf_sweep.sh` flashes one perf-scoped diagnostic image per build and layout
seed with `--hot-tunables` whenever a `--knob` is given. The plan records
this flag and the table header identifies live knobs. Each flash is one
capture of the first suite, which waits for the image to boot and is
discarded, so no point takes the first capture after a boot. Default device images
keep `TUNE_HOT` values constant. Every round visits the Cartesian product of knob values in a fresh,
recorded random order. The first build and first value of each knob form the
baseline. Each capture sets its point's knobs under its own lock
(`autana suite --set`), so whoever holds the board between two captures cannot
change what is measured. The wrapper restores `origin/main`'s release image
afterwards unless `--no-restore` is passed.

```sh
launcher/tools/perf/perf_sweep.sh --knob r3d_span.small_max_side=2,3,4 \
  --suite run_raster_scale_perf_suite counters - --runs 5
```

| Option | Meaning |
| --- | --- |
| `--out DIR` | Captures, plan and generated `sweep.md` / `sweep.json`; defaults to a temporary directory in the wrapper |
| `--build LABEL=REV` | Repeatable revision or directory; defaults to this project, labelled `here` |
| `--knob NAME=V1,V2,...` | Repeatable console tunable and integer values |
| `--suite NAME TESTS TABLE` | Repeatable suite, filters and report command with `@CAPTURE@`, `@TABLE@`, `@PROJECT@`; `-` reads raw metrics |
| `--seeds S1 S2 ...` | Distinct layout seeds; default `1` |
| `--runs R` | Rounds per flash; default `DEFAULT_RUNS` in `perf_sweep.py` |
| `--rng-seed N` | Replay the recorded point order |
| `--threshold PCT`, `--alpha P` | Equivalence margin and family error rate; defaults shared with `perf_compare.py` |
| `--timeout SECONDS`, `--wait SECONDS` | Capture and board queue deadlines |
| `--autana COMMAND` | Override acquisition, status checks and restore for host replay |
| `--dry-run` | Print flashes, capture count and point order without board access |
| `--no-restore` | Omit the wrapper's release restore |

The table has one row per metric every capture reported: timing rows, the
suite's own frame-cost stage averages, one-core span fields and per-call
counters. A knob point is compared with its build's baseline point on the same
images, so layout is shared and the interleaved runs are the observations; it
counts as improved or regressed when significant, beyond the threshold, and in
the same direction on every seed. A build is compared with the first build by
per-seed means, which carry layout noise; with one seed that comparison is
inconclusive. `seed_statistics.compare()` applies Welch, TOST, permutation
agreement and Holm across the table's cells.

`plan.json` and per-capture JSON retain failures; two consecutive capture
failures stop acquisition. Rebuild the generated report with
`python launcher/tools/perf/perf_sweep.py report DIR`.

Render perf captures force every timed render window through portrait and
landscape. Rows carry `quarter=portrait` or `quarter=landscape`; the quarter
is part of the metric name in comparisons, layout pilots and sweeps. Stage
and span metrics retain that identity too. Untagged captures retain their
unqualified row names, so comparisons do not silently pair them with tagged
rows. Single-viewport render documentation readers select portrait by default;
`dynres_report.py --quarter landscape` selects landscape explicitly.

## Nightly scoped chain

```mermaid
sequenceDiagram
    participant Marker as Hosted marker
    participant Wait as Hosted wait legs
    participant GPU as GPU stage
    participant Heavy as Heavy lock
    participant Plan as Board plan
    participant Lanes as Board lanes
    Marker->>Wait: Compute the marker in NIGHT_ZONE
    Wait->>Wait: Sleep until marker within each leg budget
    Wait->>GPU: Start work with a fresh token
    GPU->>GPU: Wait for board locks to clear
    GPU->>Heavy: Take heavy
    GPU->>GPU: Render to completion
    GPU->>Heavy: Release heavy
    Wait->>Plan: Start work with a fresh token
    Plan->>Plan: Wait for two GPU idle polls
    Plan->>Lanes: Run lanes to completion
    Lanes-->>Plan: Lane outcomes and timestamps
    Plan->>Plan: Record timeline and host free time
```

GPU work starts at `vars.NIGHT_START`; the board night starts at `vars.BOARD_START`, later.
Both accept HH:MM in 00:00–05:59; an empty value uses the constant of the same name
in `scripts/lib/local_schedule.py`. The board night has no end time: lanes run to
completion under their hang guards, and day board jobs queue on the lock behind them.

The zone is the NIGHT_ZONE repository variable (IANA name; UTC when unset).
The marker is the first local start at or after the trigger; override starts now.

Both workflows schedule at 20:17Z. The trigger is the latest daily UTC cron slot,
including when GitHub starts it late. Other events use the current time.
A passed marker starts immediately. Each date uses the configured zone's UTC offset.

The hosted marker job computes the marker and exits. Serialized hosted wait
legs sleep within `HOSTED_WAIT_SECONDS` in `scripts/lib/local_schedule.py`.
The work jobs depend on both jobs and start after the marker passes.

The GPU stage yields green when bakes are queued or running.
Every Bakes GPU completion triggers a fresh image check.

The plan waits for two polls without queued, waiting or running jobs carrying
`gpu` in either GPU workflow. Hosted jobs waiting for a later marker do not count.

This check requires a host GPU runner marker fresh within `GPU_HOST_MARKER_DAYS`.
Otherwise it reports “GPU runner not on this host” and continues.

The repository variable `REMOTE_LANE_COMMAND` names a command on the nightly's machine
that reaches other machines' boards; `REMOTE_LANE_ENV` in `nightly/scoped_chain.py` states its contract.
Unset, the night uses this machine's boards only.

A remote board's lane builds and runs on its own host. Its folder under `--out` goes there
and comes back with the lane's data and captures; the captures sync under that host's name.
`scoped_chain.py boards` lists every board the night reaches, which board health checks.

Initial boards split first-parent commits on main by position modulo their count.
Joiners use independent `0/1` series while any lane runs.
Boards repeat their last measured anchor and resume after `last_attempted`;
fresh boards start at their newest commit.

Builds take the heavy lock at below-normal priority. Step `wait_seconds`
records heavy queue time separately from `build_seconds`.
A flash is skipped when its image hash equals the board's last measured hash;
an anchor always measures.

| Guard | Owner |
|---|---|
| GPU process tree | `GPU_STAGE_HANG_SECONDS` in `scripts/lib/local_schedule.py` |
| Lock wait | `NIGHT_STEP_HANG_SECONDS` in `scripts/lib/local_schedule.py` |
| Lane process tree | `LANE_HANG_FACTOR` times the lane estimate in `nightly/scoped_chain.py` |
| Build, capture and self-test | Their timeout constants in `nightly/scoped_chain.py` |

A lane stops after `CAPTURE_FAILURE_LIMIT` consecutive capture failures or
when its board unplugs. A returning planned board does not restart.

A busy heavy lock stops the lane with `not_run_busy` and preserves its cursors.
Build failures advance `last_attempted`; measured steps advance both cursors
and `last_image_hash`. Capture failures keep the cursors.

The first board plugged into this machine runs `scripts/ci/board-steps.sh` before its chain
inside the same board hold. A night with no board at plan time fails.
Partial manifests are saved after every step and after cleanup.
Raw captures upload even when a lane fails.

The orphan `perf-data` branch holds these records:

| Path | Contents |
|---|---|
| `scoped/<board>/<plan-start>/night.json` | Lane start/end, outcomes, cursors, hashes, durations and self-test |
| `scoped/<board>/<plan-start>/<NN>-<sha12>.json` | LNT v2 reports with USB serial machine names and samples in seconds |
| `scoped/timeline/<marker>.json` | GPU and board intervals, outcomes and latest completion (`free_at`) |

`night` is the marker's date. The timeline summary reports elapsed time from
the marker until the host is free.

GPU timing records live under the shared heavy root's `night-steps/<marker>/`.
Bakes without a marker key records by the stage's start time.

For a manual chain, create a data checkout and plan, then run `night`.
`--from` includes that commit when assigned to the lane; `--max-steps`
limits new candidates and retains the anchor.

```sh
python launcher/tools/perf/nightly/scoped_chain.py plan \
  --boards BOARD_A BOARD_B --data DATA --out plan.json --marker "$MARKER" \
  --from SHA --max-steps 2
python launcher/tools/perf/nightly/scoped_chain.py night \
  --plan plan.json --data DATA --out captures --suite SUITE - -
python launcher/tools/perf/nightly/scoped_chain.py timeline \
  --plan plan.json --data DATA --summary summary.txt
```

`--suite NAME TESTS TABLE` is perf_compare's triplet: suite name,
comma-separated test patterns or `-` for all, and table command or `-` for raw
timing lines. The workflow's `NIGHTLY_SUITES` supplies these triplets.

`plan.json` records the marker, start, board lanes, estimates and selection.
Joining boards use the same selection. The workflow also runs docs retrieval
evaluations: hybrid on the GPU runner and lexical on hosted Linux.

## Perf store

`perf_index.py` keeps every board capture as one parsed line on the
`perf-data` branch, so a row's history on each board reads from one place
without the raw logs.

```sh
python launcher/tools/perf/perf_index.py sync --host <machine>
python launcher/tools/perf/perf_index.py store --path DIR
```

| Piece | Detail |
|---|---|
| file | `captures/<board>/<YYYY-MM>/<date>-<host>.jsonl`, one line per capture; each machine writes its own files, so two never conflict |
| line | board short id, `--host` name, build id, commit, suite, start time, image flags, and each row's goal and samples; never a log, path or user name |
| row | a `PERF TARGET` line (pegged or not), a `mean Nus` line or a `frame_profile:` row |
| capture | one recorded `run-suite` or `selftest` that completed |
| image | the flags its flash built with (`image_flags`); `null` for captures from before flashes recorded them |

The branch is as public as the repository, so machine names and capture
times are too. `sync` parses captures not yet in the store, commits and
pushes; one sync per store runs at a time. `--store DIR` uses an existing
checkout and `--write-only` leaves the commit and push to the caller.
`store --path DIR` checks the branch out, or starts it empty (the nightly's
first step); the nightly syncs before it publishes.

On a desk machine, run `sync` by hand, or from `lock_hook` with `--hook`:
`sync --hook --host <machine>` returns at once and, on a `released` event
only, starts the sync detached, since a hook is stopped after three seconds.
Each capture is parsed once; parsed lines are cached in the per-user autana
cache.

## Real-time playback

`frame_profile:` rows time whole app frames while a clip plays; the render_lab
suite that emits them is described in its
[tools README](../../main/apps/render_lab/tools/README.md#frame-timing-profiles).

`frame_profile:` reports frame count and mean, median, p95, worst and best
wall times. Mean FPS is timed frames / total wall time; low FPS is 1/p95,
worst FPS is 1/worst frame time. `quarter=` identifies orientation.

The warm-up runs at dt 0 and is excluded from statistics. Its wall time
supplies the first clip advance. Each later frame advances by the previous
frame's wall time.

A traced playback (`frame_profile_play` given a trace) also keeps each
frame's path time. `frame_trace:` lines list every frame and `frame_worst:`
lines rank the slowest. `frame_replay:` lines hold the cull view's counts
and a frame-cost report at one replayed pose.

The capture parser compares mean microseconds for each scenario and quarter.
The fields are defined in `launcher/test/frame_profile.h`.

Pose-time parsers accept `t=...ms`; the `t=...s` form is for older captures.

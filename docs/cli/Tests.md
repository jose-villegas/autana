# Tests

Runs suites, and other pass/fail signals from the board such as `tone`.
`autana help tests` lists the same commands. `suite` is the everyday path;
`selftest` runs every registered suite, for a full pre-merge pass.

<!-- generated: cli-tests check: python scripts/autana/cli_reference.py --check -->
| Command | What it does |
|---|---|
| `autana suite <name>... [seconds] [--runs N] [--flash] [--layout-seed N] [--hot-tunables] [--verbose]` | run suites under one lock, N times each (1 when omitted); seconds caps each capture (1800 when omitted); --flash builds and flashes the diagnostics image first, so nobody flashes between captures |
| `autana suite <name> --test PATTERN[,PATTERN]` | only the tests a pattern picks: part of a function name or a whole case segment, a/b for segments in order (case-sensitive); --test repeats |
| `autana suite <name> --send LINE` | send a console line after tunables, before every capture; --send repeats in order; the sequence should be idempotent, and an error or incomplete reply stops the batch |
| `autana suite <name> --set NAME=VALUE` | set a tunable before each capture; --set repeats |
| `autana suite list [text] [--json]` | the registered suites; [on request] ones run only by name |
| `autana selftest [seconds] [--verbose]` | build the autorun diagnostics image, flash, run every suite; 3000 s when omitted |
| `autana melody [volume] [--check] [--internal] [--json]` | development builds: play the phrase, or check every note with --check; --internal selects silent codec feedback |
| `autana tone <hz> <ms> [volume] [--internal] [--json]` | development builds: play <hz> for <ms> at volume % (firmware default 30) and report the level against a silence baseline, whether it was heard and the measured frequency; --internal uses silent codec feedback |
<!-- /generated: cli-tests -->

A run prints report and capture paths, PASS/FAIL counts and failures. A
capture that never reaches its `*_COMPLETE` line fails. `--verbose` prints it
all. `autana batch` is `suite ... --runs N --perf-scope --verbose --flash`.
`--set NAME=VALUE` takes the tunables of [Live-Tuning.md](Live-Tuning.md).

`melody` and `tone` reply as the [audio path](../audio/Audio-Path.md#melody)
documents. `tone`'s JSON adds `path`, `measured_hz` and `hz_error` (Hz,
measured minus requested); with fewer than two analysis windows the
frequency fields are null.

## A/B testing

To compare two versions, measure both on the same board under one lock and
repeat each side, because one run says little. A and B are revisions or
project directories (two git worktrees, say), each built with `autana build`.

| Route | Use it for |
|---|---|
| `--runs N --test PATTERN` | A quick look: repeat only the tests you changed. |
| `--layout-seed N` | The same code laid out differently in memory. A difference that holds across several seeds is not layout luck. |
| `launcher/tools/perf/perf_compare.sh A B --suite NAME TESTS TABLE` | A verdict: flashes seeded images of both sides in a randomised plan and says whether they differ ([perf README](../../launcher/tools/perf/README.md)). `TESTS` is a `--test` filter and `TABLE` the timing table; `-` means all or raw timings. `A A` measures the noise. Run it from Git Bash on Windows. |

`--test` takes a case-sensitive pattern; the board filters, so the capture is
much shorter than a full suite. A pattern that matches nothing fails and lists
the suite's tests. It needs a diagnostics build (`autana flash diag`).

A test name is its function name, then any case segments its table loop gave it
(`RUN_TEST_CASE`, [Writing a test](../testing/Writing-a-Test.md#conventions)):

| Pattern | Picks | Not |
|---|---|---|
| `fire` (no `/`) | part of a function name: `test_fire_fits` | |
| `scene-a` (no `/`) | a whole segment: `test_profile/scene-a/portrait` | `test_profile/scene-a-full/portrait` |
| `scene-b` | every take of a group: `test_profile/scene-b/front/portrait`, `.../side/portrait` | |
| `scene-b/front` | each part a whole segment, in order: `test_profile/scene-b/front/portrait` | `.../scene-b/side/portrait` |
| `profile/scene-b` | the first part may be part of the function name | `test_steps/scene-b/front/portrait` |

By hand, put both sides inside one `lock run` so nobody flashes between them
(`run_example_perf_suite` and `row` stand for your suite and test):

| Linux | Windows PowerShell |
|---|---|
| `autana lock run -- sh -c 'autana --project A suite run_example_perf_suite --flash --runs 3 --test row && autana --project B suite run_example_perf_suite --flash --runs 3 --test row'` | `autana lock run -- powershell -NoProfile -Command 'autana --project A suite run_example_perf_suite --flash --runs 3 --test row; if ($LASTEXITCODE) { exit $LASTEXITCODE }; autana --project B suite run_example_perf_suite --flash --runs 3 --test row; exit $LASTEXITCODE'` |

# Tests

Runs suites, and other pass/fail signals from the board such as `tone`.
`autana help tests` lists the same commands. `suite` is the everyday path;
`selftest` runs every registered suite, for a full pre-merge pass.

<!-- generated: cli-tests check: python scripts/autana/cli_reference.py --check -->
| Command | What it does |
|---|---|
| `autana suite <name>... [seconds] [--runs N] [--flash] [--layout-seed N] [--hot-tunables] [--verbose]` | run suites under one lock, N times each (1 when omitted); seconds caps each capture (1800 when omitted); --flash builds and flashes the diagnostics image first, so nobody flashes between captures |
| `autana suite <name> --test PATTERN[,PATTERN]` | only the tests whose name contains a pattern (case-sensitive); --test repeats |
| `autana suite <name> --send LINE` | send a console line after tunables, before every capture; --send repeats in order; the sequence should be idempotent, and an error or incomplete reply stops the batch |
| `autana suite <name> --set NAME=VALUE` | set a tunable before each capture; --set repeats |
| `autana suite list [text] [--json]` | the registered suites; [on request] ones run only by name |
| `autana selftest [seconds] [--verbose]` | build the autorun diagnostics image, flash, run every suite; 3000 s when omitted |
| `autana melody [volume] [--check] [--internal] [--json]` | development builds: play the phrase, or check every note with --check; --internal selects silent codec feedback |
| `autana tone <hz> <ms> [volume] [--pulse MS] [--internal] [--json]` | development builds: measure <hz> over <ms> at volume % (firmware default 30) and report the level against a silence baseline, whether it was heard and the measured frequency; --pulse MS limits playback within the measurement window; --internal uses silent codec feedback |
<!-- /generated: cli-tests -->

A run prints report and capture paths, PASS/FAIL counts and failures. A
capture that never reaches its `*_COMPLETE` line fails. `--verbose` prints it
all. `autana batch` is `suite ... --runs N --perf-scope --verbose --flash`.
`--set NAME=VALUE` takes the tunables of [Live-Tuning.md](Live-Tuning.md).

`melody` and `tone` reply as the [audio path](../audio/Audio-Path.md#melody)
documents. `tone`'s JSON adds `path`, `measured_hz` and `hz_error` (Hz,
measured minus requested); with fewer than two analysis windows the
frequency fields are null.

After the silence baseline, `--pulse MS` plays a timed SYSTEM-bus tone.
Its positive length must fit a full-queue measurement window, bounded by
`CONSOLE_TONE_PULSE_WINDOW_MAX_MS` in `console_tone_parse.h`.

Pulse capture selects IMU samples by their producer times and collects
delayed batches after the pulse span. The edges estimate audible onset.
The microphone window includes silence after playback ends.

JSON includes `pulse_ms` (zero for continuous playback), `shake_rms_mg`,
`shake_peak_mg`, and `silence_shake_rms_mg`; unavailable shake values are null.
`gx_mg`, `gy_mg`, and `gz_mg` report the silence-window accelerometer mean
in mg, including gravity; they are null without a silence reading.

## A/B testing

To compare two versions, measure both on the same board under one lock and
repeat each side, because one run says little. A and B are revisions or
project directories (two git worktrees, say), each built with `autana build`.

| Route | Use it for |
|---|---|
| `--runs N --test PATTERN` | A quick look: repeat only the tests you changed. |
| `--layout-seed N` | The same code laid out differently in memory. A difference that holds across several seeds is not layout luck. |
| `launcher/tools/perf/perf_compare.sh A B --suite NAME TESTS TABLE` | A verdict: flashes seeded images of both sides in a randomised plan and says whether they differ ([perf README](../../launcher/tools/perf/README.md)). `TESTS` is a `--test` filter and `TABLE` the timing table; `-` means all or raw timings. `A A` measures the noise. Run it from Git Bash on Windows. |

`--test` takes a case-sensitive substring; the board filters, so the capture is
much shorter than a full suite. A pattern that matches nothing fails and lists
the suite's tests. It needs a diagnostics build (`autana flash diag`).

By hand, put both sides inside one `lock run` so nobody flashes between them
(`run_example_perf_suite` and `row` stand for your suite and test):

| Linux | Windows PowerShell |
|---|---|
| `autana lock run -- sh -c 'autana --project A suite run_example_perf_suite --flash --runs 3 --test row && autana --project B suite run_example_perf_suite --flash --runs 3 --test row'` | `autana lock run -- powershell -NoProfile -Command 'autana --project A suite run_example_perf_suite --flash --runs 3 --test row; if ($LASTEXITCODE) { exit $LASTEXITCODE }; autana --project B suite run_example_perf_suite --flash --runs 3 --test row; exit $LASTEXITCODE'` |

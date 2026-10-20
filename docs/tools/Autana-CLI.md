# The autana command

One command for everything that touches the board: build and flash, run
suites, watch it, drive it, change a number live. `build` and `flash` act on
the current directory or `--project PATH`; the rest work from anywhere.

New here: set up `autana` as in [the README](../../README.md#run-it-on-the-board),
then run `autana flash dev` and `autana monitor 30` (Git Bash on Windows).
No board? Try the [host render](../../README.md#try-it-without-a-board).

## Contents

- [I want to...](#i-want-to) · [Variants](#variants) · [Flags](#flags)
- By what a command does: [Sharing the board](#sharing-the-board) ·
  [Build and flash](#build-and-flash) · [Tests](#tests)
  ([A/B testing](#ab-testing)) · [Watch the board](#watch-the-board) ·
  [Drive](#drive) · [Tune](#tune) · [Debug](#debug) ·
  [Docs](#docs)
- Reference: [JSON fields](#json-fields) · [In a session](#in-a-session) ·
  [Settings](#settings)

## I want to...

| Goal | Command |
|---|---|
| Build and flash | `autana flash dev` |
| Read the console for 30 s | `autana monitor 30` |
| See the screen | `autana screenshot -o shot` (`shot.png` plus `shot.json`) |
| Read or set a live value | `autana tune ridge_trail` |
| Run tests | `autana suite list`, then `autana flash diag` and `autana suite <name>` |
| Compare two versions for speed | [A/B testing](#ab-testing) |
| Check memory | `autana heap` |
| Pause and step the app | `autana debug freeze`, `debug step 3`, `debug resume` |
| Share one board | `autana status`; wrap work in `autana lock run -- <command>` |
| Fail a CI job rather than queue | `autana --wait 0 --owner my-job lock run -- <command>` (busy board: exit 75) |

## Variants

Builds come in `dev` (the console, for daily work), `diag` (dev plus the test
suites) and `rel` (no console). Board commands need `dev` or `diag`.

## Flags

`autana help flags` prints this table. Global flags go anywhere on the line and
are described under [Settings](#settings); each command sits in the first
section below whose rule it matches.

| Flag | What it does | Commands |
|---|---|---|
| `--lock-token TOKEN` | Run inside this live hold. | every board command |
| `--wait SECONDS` | How long to wait for the lock; 0 fails at once (exit 75). | every board command |
| `--owner NAME` | The lock owner's label. | every board command |
| `--board SERIAL` | USB serial or short id; `-b` is an alias. | every board command |
| `--out PATH` | Write the one capture here instead of the default path. | selftest, suite, monitor |
| `--expect-build-id ID` | Refuse to run unless the board, or the image `--flash` just wrote, carries this `BUILD_ID`; `flash` prints it. | suite |
| `--version` (or `-V`) | Print autana's own version and exit. | none |
| `--project PATH` | Act on `PATH` instead of the current directory; it must hold `launcher/CMakeLists.txt`. | build, flash, selftest, suite, suite list, tune save, docs |

## Sharing the board

Host only; never talks to the firmware. The lock lives on this machine, so it
guards sessions and CI jobs on one PC, not two PCs plugged into one board.

`autana help lock` · busy board, guarantees and recovery: [Device-Lock.md](Device-Lock.md)

| Command | What it does |
|---|---|
| `autana devices [--json]` | Short ids, serials, ports and availability. Same JSON as `status --json`. |
| `autana status [--json]` | Every board: free or held, the holder, estimated free time, and the queue. |
| `autana lock run [--keep-on-failure <seconds>] [--estimate <seconds>] -- <command...>` | Hold the board while the command runs; it inherits the token. |
| `autana lock take [--for <seconds> \| --until-released [--pid <pid>]] [--estimate <seconds>] <note...>` | Wait for the board and print a token. A timed hold lasts at most an hour. |
| `autana lock release [<token>]` | Release a hold early. |
| `autana lock wait` | Wait until the board is free, without taking it. |
| `autana lock hand [--until-back <seconds>] <note...>` | Reserve the board for a person for an hour; run again to renew. |
| `autana lock take-back` | Clear that reservation. |

Name a board with `--board A308`, the last four hex digits of its serial. With
several plugged in, jobs that flash pick a free one; others need `--board`. A
busy board exits 75 ([exit codes](Device-Lock.md#exit-codes-and-json-status)).

## Build and flash

Makes or writes an image, or says which image runs.

`autana help build`

| Command | What it does |
|---|---|
| `autana build [rel\|dev\|diag] [--perf-scope] [--layout-seed N] [--hot-tunables] [--configure]` | Build without a board lock; `dev` when omitted. `--layout-seed N` pads the layout ([Layout-Noise.md](Layout-Noise.md)); 0 is the plain build. `--hot-tunables` adds live hot-path knobs. `--configure` only writes `sdkconfig` and `compile_commands.json`. |
| `autana build diag --check` | The diagnostics build plus the complexity ratchet; no board. |
| `autana flash [rel\|dev\|diag] [--quiet] [--perf-scope] [--layout-seed N] [--hot-tunables]` | Build, then flash under the lock; `dev` when omitted. `--quiet` logs only. `--perf-scope` (diag) builds the perf-scoped image. |
| `autana buildid [--json]` | The `BUILD_ID` the board runs, to check against what was flashed. |

Private process-tree channels `_AUTANA_BUILD_SERIAL` (mutex directory) and
`_AUTANA_BUILD_PRIORITY=below_normal` throttle compilation across build and flash paths.
`_AUTANA_BUILD_LATEST_START` bounds lock waits and build starts by Unix time; captures retain normal priority.

`BUILD_ID` is the image's ELF hash and proves the write, not the boot
([what a flash proves](Flash-and-Captures.md#what-a-flash-proves)). Which
ESP-IDF is used and where builds live: [Build-Environment.md](Build-Environment.md).

## Tests

Runs suites, and other pass/fail signals from the board such as `tone`.

`autana help tests` · `suite` is the everyday path; `selftest` runs every
registered suite, for a full pre-merge pass.

| Command | What it does |
|---|---|
| `autana suite <name>... [seconds] [--runs N] [--flash] [--layout-seed N] [--hot-tunables] [--verbose]` | Run suites under one lock, `N` times each (1 when omitted), `seconds` capping each capture (1800 when omitted). `--flash` builds and flashes the diagnostics image first, so nobody flashes between captures. |
| `autana suite <name> --test PATTERN[,PATTERN]` | Only tests whose name contains a pattern; `--test` repeats. |
| `autana suite <name> --set NAME=VALUE` | Set a tunable ([Live-Tuning.md](Live-Tuning.md)) before each capture; `--set` repeats. |
| `autana suite <name> --send LINE` | Send a console line after tunables, before every capture; `--send` repeats in order. The sequence should be idempotent; an error or incomplete reply stops the batch. |
| `autana suite list [text] [--json]` | The registered suites; `[on request]` ones run only by name. |
| `autana selftest [seconds] [--verbose] [--perf-scope] [--out PATH]` | Build the autorun diagnostics image, flash, run every suite; 3000 s when omitted. |
| `autana tone <hz> <ms> [volume] [--internal] [--json]` | Development builds only: plays `<hz>` for `<ms>` at `[volume]` % (firmware default 30) and reports the tone level against a silence baseline in dBFS, whether it was heard, and the DMA bytes held while open. `--internal` uses silent codec feedback. Both paths report measured frequency; JSON adds `path`, `measured_hz`, and `hz_error` (Hz, measured minus requested). Fewer than two analysis windows gives `measured n/a` and null JSON frequency fields. |

A run prints report and capture paths, PASS/FAIL counts and failures. A
capture that never reaches its `*_COMPLETE` line fails. `--verbose` prints it
all. `autana batch` is `suite ... --runs N --perf-scope --verbose --flash`.

### A/B testing

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

## Watch the board

Only reads the board, so running a command twice changes nothing.

`autana help watch`

| Command | What it does |
|---|---|
| `autana monitor [seconds] [--follow] [--stream] [--elf PATH]` | In a terminal: the console live, until Ctrl+C or for `seconds`. Piped: needs `seconds` or `--follow`, and prints only errors and `FRAME_WATCH` warnings unless `--stream`. Exits 1 on a crash or reboot. |
| `autana coredump [--elf PATH]` | The last panic's core dump, saved to the records and decoded against the build that crashed. Resets the board ([Debugging](../notes/Debugging.md#after-a-crash-the-reset-reason-and-the-core-dump)). |
| `autana coredump --core FILE [--elf PATH]` | Decode a saved dump. No board. |
| `autana coredump --erase` | Clear the dump so the next crash's is told apart. Resets the board. |
| `autana screenshot [--as-shown\|--framebuffer] [-o PATH]` | `PATH.png` plus a `PATH.json` state snapshot. Landscape by default. |
| `autana screenshot --frames N -o PATH` | `N` frames as `PATH-00` on: the loop is frozen and stepped a frame between captures, then resumed. |
| `autana battery [--json]` | Fresh battery voltage, percentage, USB, charge phase and flow, plus raw PMU status bytes; development builds. |
| `autana heap [--json]` | Free bytes and largest block in the internal DMA pool (`MEMORY_DMA`). |
| `autana framewatch` | The frame watch as JSON: allocations, frees and log lines repeating frame after frame ([the frame watch](../Firmware-Architecture.md#the-frame-watch-no-allocating-or-logging-in-steady-state)). |
| `autana perf` | The `frame_cost` names seen so far and the counter events. |
| `autana perf <name> [event] [seconds]` | Count one event (`insn` when omitted) on one `frame_cost` bracket for `seconds` (10 when omitted) and print cycles avg/min/max, `n` and cycles per event ([Frame-Cost.md](Frame-Cost.md#hardware-counters)). |
| `autana perf off` | Disarm the counters. |

## Drive

Changes state the way a person holding the board could.

`autana help drive`

| Command | What it does |
|---|---|
| `autana tap <x> <y>` | Tap, 50 ms. |
| `autana press <x> <y> [ms]` | Hold; 1000 ms when omitted. |
| `autana drag <x0> <y0> <x1> <y1> <ms>` | Drag between two points over `ms`. |
| `autana button <boot\|power> [short]` | A BOOT or PWR press; short when omitted. |
| `autana button <boot\|power> long` | A long press. |
| `autana reset [--capture [seconds]] [--verbose]` | Reboot and wait for USB serial; `--capture` records the boot (20 s). |
| `autana apps` | The registered apps, and which is running. |
| `autana apps --json` | The same, as JSON. |
| `autana open <name>` | Enter an app; case-insensitive unambiguous prefix. |
| `autana home` | Back to the launcher. |
| `autana sound play <name> [loop] [gain]` | Play a sound from the engine pack; gain is a percent, full gain when omitted. |
| `autana sound stop [voice\|all]` | Stop a voice or all voices; all when omitted. |
| `autana sound stats` | Voice states, dropped commands, amplifier state and mixer RAM sizes. |

Coordinates are pixels of the default `autana screenshot`, whichever way the
board is held; read a button's centre off that image. Pixels from an
`--as-shown` or `--framebuffer` capture are in another frame and may miss.

## Tune

Changes a `TUNE()` value.

`autana help tune` · what makes a constant tunable: [Live-Tuning.md](Live-Tuning.md)

| Command | What it does |
|---|---|
| `autana tune [text] [--json]` | List the tunables with ranges; names containing `text`. |
| `autana tune <name> [value]` | Show one, or set it on the board (lost on reboot). |
| `autana tune reset <name>` | Back to the value the source declares. |
| `autana tune save` | Write the board's values into this project's `TUNE(...)` lines. |

## Debug

Intervention only: it suspends or steps the frame loop, or overrides sensors
below the input layer, with a matching resume or release. Hidden from bare
`autana help`; `autana help debug` lists it.

| Command | What it does |
|---|---|
| `autana debug freeze` | Stop the frame loop where it is. |
| `autana debug resume` | Run it again. |
| `autana debug step [N]` | Advance `N` frames while frozen; 1 when omitted. |
| `autana debug touch <down\|up> <x> <y>` | One raw touch level at a panel-frame point (a `--framebuffer` pixel); `up` hands back to the controller. |
| `autana debug imu <ax> <ay> <az>` | Raw accelerometer counts; `autana debug imu quarter <q>` turns the screen to display quarter q, and `autana debug imu release` hands back to the sensor. |

`freeze`, `resume`, `step`, `touch`, `imu` and `debug framewatch` are accepted
as aliases and print the canonical spelling once.

## Docs

`autana help docs` · how it ranks and what it needs: [Docs-Search.md](Docs-Search.md)

| Command | What it does |
|---|---|
| `autana docs <question...>` | The three best sections, excerpted with their `path:line`, then five more to read on. |
| `autana docs --section <path:line>` | One section whole, or `path#heading words`; `--deep` adds subsections. |
| `autana docs --outline <path>` | A document's headings with lines and sizes. |
| `autana docs --ask <question...>` | A short answer from the local chat model, with sources. |

## JSON fields

| Command | Fields |
|---|---|
| `status` | `boards`, one object per board ([Device-Lock.md](Device-Lock.md#exit-codes-and-json-status)). |
| `buildid` | `build_id` |
| `tone` | `hz`, `tone_db`, `silence_db`, `heard`, `open_dma_bytes` |
| `heap` | `free`, `largest`, `total` (bytes in `MEMORY_DMA`) |
| `framewatch` | `frames`, `allocs`, `frees`, `console`, `repeating`, `dropped`, `sites` |
| `apps` | `apps`: `name`, `running` |
| `suite list` | `suites`: `name`, `source`, `on_request`, `device_only` |
| `tune` | `tunables`: `name`, `value`, `min`, `max`, `default` |

A filter that matches nothing gives an empty array.

## In a session

`autana` alone starts a session: the same commands without the prefix. Any
other line goes to the board as typed, so an app's own command works
([App-Commands.md](App-Commands.md)). `use A308` pins a board; `use -` clears it.

```
autana> tune theme_rgb 0x1199C8
autana> debug freeze
autana> debug step 3
autana> debug resume
autana> quit
```

## Settings

A setting is a global option before the command, or a key in the optional,
gitignored `autana.local.toml` in the project folder, which covers every
command run from that checkout. `autana help config` lists the keys.

A CI job that should fail rather than queue writes `autana --wait 0 <command>`
and names itself with `--owner`. The other global flags are in [Flags](#flags).

| Key | Meaning |
|---|---|
| `idf_installs` | ESP-IDF checkouts searched before known installs ([Build-Environment.md](Build-Environment.md)). |
| `build_root` | External build root. |
| `docs_extra` | Extra Markdown files or folders `autana docs` searches. |
| `records` | Where captures and `index.jsonl` land; `.records/device` without it ([Flash-and-Captures.md](Flash-and-Captures.md#where-a-capture-lands)). |
| `lock_hook` | A shell command run on every lock event ([Device-Lock.md](Device-Lock.md#lock-events)). |

```toml
idf_installs = ['C:\Espressif\v6.1']
build_root = 'C:\autana-builds'
docs_extra = ["notes/bench.md", "~/Documents/design-notes"]
records = "~/autana-records"
lock_hook = "python ~/bin/on_lock.py"
```

The file takes `key = "..."` lines (`'...'` keeps a Windows path as typed) and
one-line arrays of them; anything else is refused. No environment variable is a
setting. The commands live in `scripts/autana/autana.py`.

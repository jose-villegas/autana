# The autana command

One command for everything that touches the board: build and flash, run
suites, watch it, drive its input, change a number live. It takes the device
lock. A board-only command (`monitor`, `tap`, `tune`, ...) works from any
directory; `build` and `flash` act on the current directory, like `make -C`,
or on `--project PATH`.

New here: install ESP-IDF and set up `autana` as in
[the README](../../README.md#run-it-on-the-board), then run `autana flash dev`
and `autana monitor 30`. On Windows run `autana` (and the `.sh` tools it
names) from Git Bash. No board? Try the
[host render](../../README.md#try-it-without-a-board).

```sh
autana                  # a session: the same commands without the prefix
autana help [topic]     # the list below; a topic is a group key or a command
```

Builds come in three variants: `dev` (the console, for daily work), `diag`
(dev plus the test suites) and `rel` (release, no console), so commands that
talk to the board need `dev` or `diag`. Coordinates are
panel pixels. Tab completes command names and help topics where Python has
`readline` (Windows: `pip install pyreadline3`). The launchers need the Python
in `scripts/lib/python-min-version`; CI uses the `.python-version` pin.

## Contents

- [I want to...](#i-want-to) · [Flags](#flags)
- Commands, by what they do: [Sharing the board](#sharing-the-board) ·
  [Build and flash](#build-and-flash) · [Tests](#tests)
  ([A/B testing](#ab-testing)) ·
  [Watch the board](#watch-the-board) · [Drive](#drive) · [Tune](#tune) ·
  [Debug](#debug) · [No board needed](#no-board-needed)
- Reference: [JSON fields](#json-fields) · [In a session](#in-a-session) ·
  [Setup and records](#setup-and-records) · [Settings](#settings) ·
  [Adding a command from an app](#adding-a-command-from-an-app)

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

## Flags

`autana help flags`. These stay off each command's usage line.

| Flag | What it does | Commands |
|---|---|---|
| `--lock-token TOKEN`, `--wait SECONDS`, `--owner NAME`, `--board SERIAL` (`-b`) | Global; see [Settings](#settings). | every board command |
| `--out PATH` | Write the one capture here instead of the default path. With several suites or `--runs` above 1, only valid when one suite runs once. | selftest, suite, monitor |
| `--expect-build-id ID` | Refuse to run unless the board, or the image `--flash` just wrote, carries this `BUILD_ID`. `flash` prints the one it wrote; pass it here so a board someone else reflashed is not measured. | suite |
| `--project PATH` | Act on `PATH` instead of the current directory, like `make -C`. `PATH` must itself hold `launcher/CMakeLists.txt` (no parent search); so must the current directory without `--project`, except for `suite` without `--flash`. | build, flash, selftest, suite, suite list, tune save, docs |

Commands sit in the first section below whose rule they match. Tests and
Docs cover suite runs and documentation lookup.

## Sharing the board

Host only; never talks to the firmware. The lock lives on this machine, so it
guards sessions and CI jobs on one PC, not two PCs plugged into one board.

`autana help lock` · busy board, guarantees and recovery: [Device-Lock.md](Device-Lock.md)

| Command | What it does |
|---|---|
| `autana devices [--json]` | Short ids, serials, ports and availability on this machine. Same JSON as `status --json`. |
| `autana status [--json]` | Every board, plugged in or locked: free or held, the holder, elapsed and estimated free time, and the queue with estimated starts. A board off USB is listed without a port. |
| `autana lock run [--keep-on-failure <seconds>] -- <command...>` | Hold the board while the command runs; it inherits the lock token. A failed command can leave a timed hold for inspection. |
| `autana lock take [--for <seconds> \| --until-released [--pid <pid>]] [--estimate <seconds>] <note...>` | Wait for the board and print a token for this session's commands. A timed hold lasts at most an hour; an until-released hold ends when its owner process dies. |
| `autana lock release [<token>]` | Release a hold early, given its token, or without one from inside the command that holds it. |
| `autana lock wait` | Wait until the board is free without taking it or queueing; uncapped without `--wait`. |
| `autana lock hand [--until-back <seconds>] <note...>` | Reserve the board for a person for an hour; run again to renew. With `--until-back`, wait until it is taken back or lapses. |
| `autana lock take-back` | Clear that reservation. |

Pick a board with `--board A308` or `-b A308`: the last four hex digits of
`90:70:69:FE:A3:08`. Full serials and short ids ignore case. A short id
resolves against USB boards and the lock store's known boards; a collision
needs the full serial. The USB serial names the board, so a lock survives COM
number changes.

With several boards plugged in and none selected, `flash`, `suite --flash`,
`selftest`, `batch`, `lock run` and `lock take` pick one: a free board with
nobody queued, else the earliest estimated start, else the shortest queue,
avoiding human reservations when another board is free. The choice and reason
print first and the job keeps that serial. Other board commands need `--board`,
`-b` or a console pin.

A command that loses its lock stops with `device lock was lost`; one that
finds the board busy exits 75 ([exit codes](Device-Lock.md#exit-codes-and-json-status)).
A separate `flash` then `suite` leaves a gap where another session can flash;
`suite --flash` and `selftest` hold one lock across both. `--estimate` only
feeds `status --json`'s `estimated_free` and `estimated_start`; it never ends a
hold. More: [Device-Lock.md](Device-Lock.md),
[Flash-and-Captures.md](Flash-and-Captures.md#wait-estimates).

`autana lock hand --until-back 30 put the board in download mode` pauses a
flash script until someone does that and runs `autana lock take-back`; exit 0
means the reservation was released or lapsed.

## Build and flash

Makes or writes an image, or says which image runs.

`autana help build`

| Command | What it does |
|---|---|
| `autana build [rel\|dev\|diag] [--perf-scope] [--layout-seed N] [--hot-tunables] [--configure]` | Build this project, no board and no lock; `dev` when omitted. `--hot-tunables` (dev/diag): live hot-path knobs; `suite` then needs `--flash`. `--layout-seed N`: pad the layout by seed N ([Layout-Noise.md](Layout-Noise.md)); 0 is the plain build. `--configure`: only write `sdkconfig` and `compile_commands.json`. Prints the log path and verdict, and the failing lines on failure; exits with the build's status. |
| `autana build diag --check` | The diagnostics build plus the complexity ratchet (`launcher/tools/build/build_diag_check.sh`); no board. |
| `autana flash [rel\|dev\|diag] [--quiet] [--perf-scope] [--layout-seed N] [--hot-tunables]` | Build and flash; `dev` when omitted. `--quiet`: output to the log only. `--perf-scope` (diag): the perf-scoped image, no suite run. |
| `autana buildid [--json]` | The `BUILD_ID` the board runs, to check against what was flashed. |

`build` runs `launcher/tools/build/build.sh`, holding only the build directory,
and CI builds through it too. `flash` builds with no board lock, snapshots the
image, then under the lock `scripts/device/flash_image.sh` writes it, the only
script that opens the port. `BUILD_ID` is the image's ELF hash and proves the
write, not the boot ([what a flash proves](Flash-and-Captures.md#what-a-flash-proves)).
Both print a banner with the project and variant, plus branch, commit and
dirty state when git answers.

### Which ESP-IDF

`launcher/tools/build/espressif.py` picks the checkout, names it and its paired
tools on every build, and prints a drift note when it is off-pin.

| Order | Checkout selection |
|---|---|
| Process `IDF_PATH` | Any commit. On Windows the installer's stored value does not count as an override; on Linux a profile sourcing `export.sh` does. |
| `idf_installs` | Checkouts or their parent folders, in list order, matching `launcher/idf-pin.json`. |
| Installer `IDF_PATH` (Windows) | A candidate matching the pin, HKCU before HKLM. |
| Known folders | Under `C:\Espressif` (including `frameworks/`) and `~/esp`, matching the pin. |

With no match, a build stops and prints the clone, checkout, submodule and
install commands. Tools come from `IDF_TOOLS_PATH`, else the checkout's sibling
`tools/`, the installer's tools root, then `~/.espressif`, and need a Python
environment for the checkout's major.minor; a missing one prints its install
command. autana never changes environment variables or the registry. To
override for one command:

```sh
IDF_PATH="$HOME/esp/custom-idf" autana build dev
```

```powershell
powershell -NoProfile -Command { $env:IDF_PATH = 'C:\Espressif\my-idf'; autana build dev }
```

### Where builds live

Windows builds use short paths to stay under object-path limits:

```text
launcher\build.dev -> %LOCALAPPDATA%\autana\b\<hash>\build.dev
                                             <hash>\owner.txt
```

`owner.txt` names the launcher directory the hash comes from. `build_root`
picks another root. Linux builds in-tree unless `build_root` is set, then the
in-tree names become symlinks (junctions on Windows); ESP-IDF gets the real
path for `-B` and `SDKCONFIG`.

The first build replaces an in-tree build folder with a link to the external
one, and builds delete folders of launcher directories that no longer exist.
Each variant records its ESP-IDF path and commit after a successful configure
or build. A folder configured for another ESP-IDF is emptied, except
`build.log` and the build lock; so is an unstamped non-empty one, once.

## Tests

`autana help tests` · `suite` is the everyday path; `selftest` runs every
registered suite, for a full pre-merge pass.

| Command | What it does |
|---|---|
| `autana suite <name>... [seconds] [--runs N] [--flash] [--layout-seed N] [--hot-tunables] [--verbose]` | Run suites under one lock, `N` times each (1 when omitted), `seconds` capping each capture (1800 when omitted; a board silent for 300 s ends one sooner). Without `--flash` it uses the image on the board and, on a non-diagnostics image, says so and names the fix (`autana flash diag`). With `--flash` it builds and flashes the diagnostics image first, so nobody flashes between captures. |
| `autana suite <name> --test PATTERN[,PATTERN]` | Only tests whose name contains a pattern; `--test` repeats. |
| `autana suite <name> --set NAME=VALUE` | Set a tunable ([Live-Tuning.md](Live-Tuning.md)) under each capture's lock, just before it runs; `--set` repeats. A value the board refuses or does not echo stops the batch. |
| `autana suite list [text] [--json]` | The registered suites; `[on request]` ones run only by name. |
| `autana selftest [seconds] [--verbose] [--perf-scope] [--out PATH]` | Build the autorun diagnostics image, flash, run every suite; 3000 s when omitted. |

Each run prints the report and capture paths, PASS/FAIL counts, up to ten
failure messages (then a count per suite) and the end reason. A capture
without its `SELFTEST_COMPLETE` or `RUNSUITE_COMPLETE` line fails with that
reason. `--verbose` prints the whole capture; to search it, grep the file.
One suite run once makes exactly one capture and one report.

`autana batch <suite>... [--runs N] [--perf-scope] [--verbose]` still works:
the old spelling of `suite <suite>... --runs N --perf-scope --verbose --flash`
(`--runs` defaults to 3 here), printing the new form once.

### A/B testing

To compare two versions of the code, measure both on the same board under one
lock and repeat each side, because one run says little. A and B are two
revisions or two project directories (for example two git worktrees), each
built beforehand with `autana build`. Start with a scoped run; use
`perf_compare.sh` when you need a verdict.

| Route | Use it for |
|---|---|
| `--runs N --test PATTERN` | A quick look: repeat only the tests you changed. A filtered capture takes minutes, not a full suite's quarter hour. |
| `--layout-seed N` | The same code laid out differently in memory. Speed can shift with where code lands, so a difference that holds across several seeds is real. |
| `launcher/tools/perf/perf_compare.sh A B --suite NAME TESTS TABLE` | The verdict: flashes seeded images of both sides in random order and reports whether they differ ([perf README](../../launcher/tools/perf/README.md)). `NAME` is a suite, `TESTS` a `--test` filter and `TABLE` the timing table, `-` for all. `A A` measures the noise. Run it from Git Bash on Windows. |

`--test PATTERN` is a case-sensitive substring of letters, digits and
underscores; repeat the flag or comma-separate (`--test fire,gas`). The board
does the filtering and refuses patterns past its limits
(`launcher/test/suites.h`). A pattern that matches nothing fails after the
capture and lists the suite's test names, ending a `--runs` batch there. The
report names the filter and counts only the tests that ran, so a later
comparison knows its rows. It needs a diagnostics build that takes patterns
(`autana flash diag`) and says so on an older one.

By hand, put both flashes and captures inside one `lock run` so nobody flashes
between them (`run_example_perf_suite` and `row` stand for your suite and test):

| Linux | Windows PowerShell |
|---|---|
| `autana lock run -- sh -c 'autana --project A suite run_example_perf_suite --flash --runs 3 --test row && autana --project B suite run_example_perf_suite --flash --runs 3 --test row'` | `autana lock run -- powershell -NoProfile -Command 'autana --project A suite run_example_perf_suite --flash --runs 3 --test row; if ($LASTEXITCODE) { exit $LASTEXITCODE }; autana --project B suite run_example_perf_suite --flash --runs 3 --test row; exit $LASTEXITCODE'` |

## Watch the board

Only reads the board, so running a command twice changes nothing.

`autana help watch`

| Command | What it does |
|---|---|
| `autana monitor [seconds] [--follow] [--stream] [--elf PATH]` | In a terminal: the console live, until Ctrl+C or for `seconds`. Piped or scripted: needs `seconds` or `--follow` (else a usage error, not a hang) and prints only error lines and `FRAME_WATCH` warnings; `--stream` prints everything. Ends with its capture path; exits 1 on a crash or reboot, a manual reset included. Addresses decode against `PATH`, or the build whose `build_id.txt` matches. |
| `autana coredump [--elf PATH]` | The last panic's core dump from the `coredump` partition, decoded against the matching build or `PATH`. Resets the board. See [Debugging](../notes/Debugging.md#after-a-crash-the-reset-reason-and-the-core-dump). |
| `autana coredump --erase` | Clear the dump so the next crash's is told apart. Resets the board. |
| `autana screenshot [--as-shown\|--framebuffer] [-o PATH]` | `PATH.png` plus a `PATH.json` state snapshot. Landscape by default; `--as-shown` uses the board's orientation, `--framebuffer` the raw bytes. |
| `autana screenshot --frames N -o PATH` | `N` frames as `PATH-00` to `PATH-<N-1>`: one capture while running, then the loop frozen and stepped a frame between captures, then resumed.  |
| `autana heap [--json]` | Free bytes and largest block in the internal DMA pool (`MEMORY_DMA`); development builds. |
| `autana framewatch` | The frame watch as JSON: the last frame's allocations, frees and log lines, and sites repeating frame after frame ([the frame watch](../Firmware-Architecture.md#the-frame-watch-no-allocating-or-logging-in-steady-state)). |
| `autana perf` | The `frame_cost` names seen so far and the counter events. |
| `autana perf <name> [event] [seconds]` | Arm the S3's cycle counter and one event (`insn` when omitted) on one `frame_cost` bracket, read it for `seconds` (10 when omitted), disarm, and print one summary: cycles avg/min/max, the event's average, `n` and cycles per event. Frame core only; see [Frame-Cost.md](Frame-Cost.md#hardware-counters). |
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
| `autana reset [--capture [seconds]] [--verbose]` | Reboot and wait for USB serial. `--capture` records the boot (20 s) and prints its path and any error lines. |
| `autana apps` | The registered apps, and which is running. |
| `autana apps --json` | The same, as JSON. |
| `autana open <name>` | Enter an app, even while frozen; case-insensitive unambiguous prefix. |
| `autana home` | Back to the launcher. |

A point is a pixel of the default `autana screenshot`, whichever way the board
is held: read a button's centre off that image and pass it as is. autana
converts it to the panel's own frame (the one `--framebuffer` shows), so
pixels from an `--as-shown` or `--framebuffer` capture may miss. The raw
levels below gesture, `touch` and `imu`, are under [`autana debug`](#debug).

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
| `autana debug touch <down\|up> <x> <y>` | One raw touch-controller level at a panel-frame point (a `--framebuffer` pixel, not a screenshot one); `up` hands back to the controller. |
| `autana debug imu <ax> <ay> <az>` | Raw accelerometer counts; `autana debug imu release` hands back to the sensor. |

The old `autana freeze`, `resume`, `step`, `touch` and `imu`, and `autana debug
framewatch`, still work and print the new spelling once.

## No board needed

`autana help docs` · how it ranks and what it needs: [Docs-Search.md](Docs-Search.md)

| Command | What it does |
|---|---|
| `autana docs <question...>` | The three best sections, excerpted with their `path:line` range and cited code, then five more to read on. |
| `autana docs --section <path:line>` | One section whole, or `path#heading words`; `--deep` adds subsections. |
| `autana docs --outline <path>` | A document's headings with lines and sizes. |
| `autana docs --ask <question...>` | A short answer from the local chat model, with sources. |
| `autana --version` (or `-V`) | autana's own version; it rides in a lock record so a refusal can name the holder ([Device-Lock.md](Device-Lock.md#one-copy-of-the-tools)). |

## JSON fields

| Command | Fields |
|---|---|
| `status` | `boards`, one object per board; see [Device-Lock.md](Device-Lock.md#exit-codes-and-json-status). |
| `buildid` | `build_id` |
| `heap` | `free`, `largest`, `total` (bytes in `MEMORY_DMA`) |
| `framewatch` | `frames`, `allocs`, `frees`, `console`, `repeating`, `dropped`, `sites` |
| `apps` | `apps`: `name`, `running` |
| `suite list` | `suites`: `name`, `source`, `on_request`, `device_only` |
| `tune` | `tunables`: `name`, `value`, `min`, `max`, `default` |

A filter that matches nothing gives an empty array.

## In a session

`use A308` pins the session to that board and the prompt becomes
`autana@A308>`; `use` prints the pin and `use -` clears it. `autana -b A308`
starts pinned. The pin is the session's `--board`, and a line can override it
with its own flag.

A bare word is a command above; any other line goes to the board as typed, so
an app's own command works too. A tunable is reached only through `tune`:

```
autana> tune theme_rgb 0x1199C8
autana> debug freeze
autana> debug step 3
autana> debug resume
autana> quit
```

## Setup and records

| Path | What it is |
|---|---|
| `tools/autana`, `tools/autana.cmd` | The launchers; `tools/` goes on the PATH. |
| `scripts/autana/autana.py` | Every command; `COMMAND_GROUPS` is the list above. |
| `scripts/device/device.py` | The serial port and the device lock. Nothing else opens the port. |
| `scripts/add-tools-to-path.sh [--check]` | Put `tools/` on the PATH, from the primary checkout (a worktree's entry dies with it). |

The lock is one per machine, in the system temp folder. Each session writes a
log and manifest under the `records` setting, or the checkout's gitignored
`.records/device`
([Flash-and-Captures.md](Flash-and-Captures.md#where-a-capture-lands)).

## Settings

No environment variable is a setting (`_AUTANA_*` names are plumbing to
children; system ones such as `IDF_TOOLS_PATH` are still read). A setting is a
global option before the command, covering that call, or a key in
`autana.local.toml` in the project folder, covering every command from that
checkout. The file is optional and gitignored; `autana help config` lists its
keys.

| Global option | Sets |
|---|---|
| `--lock-token TOKEN` | Run a board command inside the live hold `TOKEN` names. A wrong, released or lapsed token fails at once with exit 1. A holder's children inherit it. |
| `--wait SECONDS` | How long a board command waits for the lock across all its steps: 600 s without it, `0` fails at once with exit 75. `lock wait` is uncapped without it. A console line (`tune`, `buildid`, `screenshot`, a line typed raw) is refused at once without it rather than waited for. |
| `--owner NAME` | The name this run holds the lock under, as `NAME:<pid>` (`<user>@<host>:<pid>` without it). Name each CI job. |
| `--board SERIAL` | Full USB serial or short id, also `-b`; without it see [Which board](Device-Lock.md#which-board). |

Global flags, `--project` included, go anywhere on the line: `autana flash
diag --wait 0` and `autana --wait 0 flash diag` are the same. A CI job that
should fail rather than queue writes `autana --wait 0 <command>`.

| Key | Meaning |
|---|---|
| `idf_installs` | ESP-IDF checkouts or folders holding `esp-idf/`, searched in list order before known installs. |
| `build_root` | External build root; see [Where builds live](#where-builds-live). |
| `docs_extra` | Extra Markdown files or folders `autana docs` searches, relative to the project. |
| `records` | Where captures and `index.jsonl` land; `.records/device` without it. |
| `lock_hook` | A shell command run on every device lock event ([events and environment](Device-Lock.md#lock-events)). |

```toml
idf_installs = ['C:\Espressif\v6.1']
build_root = 'C:\autana-builds'
docs_extra = ["notes/bench.md", "~/Documents/design-notes"]
records = "~/autana-records"
lock_hook = "python ~/bin/on_lock.py"
```

The file takes `key = "..."` lines (`'...'` keeps a Windows path as typed) and
one-line arrays of such strings; anything else, or another key, is refused with
the file and line. The lock folder and the docs model's folder and port are
machine facts shared by every checkout, so they are not settings.

## Adding a command from an app

An app answers its own commands without joining the console's verb list
(`launcher/main/console/`, one file per verb):

```c
static bool
app_example_console(const char* args) {
    if (strcmp(args, "status") != 0) {
        return false;
    }
    printf("EXAMPLE status=ok\n");
    printf("EXAMPLE_END\n");
    return true;
}

APP_CONSOLE("example", app_example_console);

app_t app_example = {
    .name = "Example",
    ...
    .console = APP_CONSOLE_PTR(app_example_console),
};
```

- `APP_CONSOLE()` declares the prefix once, before the `app_t`. A prefix that
  clashes with a verb or another app's fails the boot.
- The handler gets what follows the prefix: `autana example status` (or
  `example status` in a session) calls it with `"status"`.
- Every reply line starts with the prefix in capitals; the last is
  `<PREFIX>_END`, which lets autana return at once.
- Return `false` to decline; the shell answers `<PREFIX>_ERR not handled`. An
  app that is not running gets `<PREFIX>_ERR not running`.
- It runs on the frame loop after the app's `frame()`, and while frozen. Lines
  faster than frames queue up to `APP_LINE_QUEUE_LEN`; one past that is dropped
  with no reply.
- No `#if` needed: `APP_CONSOLE_PTR()` is `NULL` in release and the handler
  goes with it.

An app that adds a command documents it itself.

# The autana command

One command for everything that touches the board: build and flash, run
suites, watch it, drive its input, change a number live. It takes the device
lock and is not tied to git or worktrees - a board-only command (`monitor`,
`tap`, `tune`, ...) works from any directory, and a command that builds or
flashes acts on the current directory, like `make -C`, or on `--project PATH`.

For a first board run, install ESP-IDF and set up `autana` as described in
[the README](../../README.md#run-it-on-the-board), then use `autana flash dev`
and `autana monitor 30`. On Windows, run these in Git Bash. The CLI handles
the ESP-IDF build environment and serial port; `monitor 30` exits after 30
seconds. For a result without a board, use the
[host render](../../README.md#try-it-without-a-board).

```sh
autana                  # a session: the same commands without the prefix
autana help [topic]     # the list below; a topic is a group key or a command
```

Only a development build answers (release has no console). Coordinates are
panel pixels. Tab completes command names, `build` and `flash` variants and help topics
where Python has `readline` (Windows: `pip install pyreadline3`).

## Most used

The five commands almost every session starts with, each a real example
rather than a placeholder - `autana help`'s own first block, ahead of every
group below.

| Command | What it does |
|---|---|
| `autana flash dev` | Build and flash; the everyday form. |
| `autana monitor 30` | The console for 30 s. |
| `autana suite list` | What this project can run, then `autana suite <name>` to run one. |
| `autana tune ridge_trail` | A live value, read or set. |
| `autana screenshot -o shot` | The panel as `shot.png` plus `shot.json`. |

## Flags

`autana help flags` - left off each command's own usage line below to keep
it readable; a flag works the same wherever the table below says it applies.

| Flag | What it does | Commands |
|---|---|---|
| `--wait SECONDS` | Global; see [Settings](#settings). | every board command |
| `--owner NAME` | Global; see [Settings](#settings). | every board command |
| `--board SERIAL` | Global; see [Settings](#settings). | every board command |
| `--out PATH` | Write the one capture here instead of the default path; with several suites or `--runs` above 1, only makes sense on `suite` when exactly one suite runs once. | selftest, suite, monitor |
| `--expect-build-id ID` | Refuse to run a suite unless the board, or the image `--flash` just wrote, carries this `BUILD_ID`. `autana flash` prints the `BUILD_ID` it just wrote once esptool's hash verifies it - pass that value here to refuse measuring a board that has since been reflashed by someone else. | suite |
| `--project PATH` | Act on `PATH` instead of the current directory - like `make -C`/`idf.py -C`, no searching parent directories. `PATH` must itself carry `launcher/CMakeLists.txt`; the current directory must too when `--project` is omitted, for every command below except `suite` without `--flash`, which only wants it for its capture's own record. Popped once ahead of any command's own parsing, so it works the same everywhere it applies. | build, flash, selftest, suite, suite list, tune save, docs |

## Build and flash

`autana help build`

| Command | What it does |
|---|---|
| `autana build [rel\|dev\|diag] [--perf-scope]` | Build this project, no board and no lock; `dev` when omitted. Prints the build's log path and verdict, and its failing lines on a failure; exits with the build's status. |
| `autana build diag --check` | The diagnostics build plus the complexity ratchet - `launcher/tools/build/build_diag_check.sh`, unchanged; no board. |
| `autana flash [rel\|dev\|diag] [--quiet] [--perf-scope]` | Build and flash this project; `dev` when omitted. `--quiet`: output to the log only. `--perf-scope` (diag): the perf-scoped image, no suite run. |
| `autana buildid [--json]` | The `BUILD_ID` the board is running, to check against what was flashed. |

`autana build` is the way to build: it runs `launcher/tools/build/build.sh`,
holding only the build directory so two builds of one variant take turns, and
CI builds through it too. `autana flash` runs that same build with no board
lock held and snapshots the image it built, then, under the board's lock,
`scripts/device/flash_image.sh` writes that snapshot - the one script that
opens the port.
`BUILD_ID` identifies the image by its ELF hash; see
[what a flash proves](Flash-and-Captures.md#what-a-flash-proves).
It proves the write, not the boot.

Both print a banner naming the project and variant; when git answers for
that project it adds the branch, commit and whether it is dirty - never
required, so a project built from a tarball or a non-git checkout still
builds and flashes.

## Tests

`autana help tests` · `suite` is the everyday path - `selftest` is every
suite this project registers, for a full pre-merge pass.

| Command | What it does |
|---|---|
| `autana suite <name>... [seconds] [--runs N] [--flash] [--verbose]` | Run one or more registered suites under one lock, `N` times each (1 when omitted), `seconds` capping each capture (1800 s when omitted; a board that goes silent for 300 s ends one sooner). Without `--flash`: against the image already on the board - `autana suite <name>` against a non-diagnostics image says so plainly and names the fix (`autana flash diag`). With `--flash`: build and flash the diagnostics image first, so nobody else can flash between two captures. |
| `autana suite <name> --test PATTERN[,PATTERN]` | Only the tests of that suite whose name contains a pattern; `--test` repeats. |
| `autana suite list [text] [--json]` | The suites this project registers; `[on request]` ones run only by name. |
| `autana selftest [seconds] [--verbose] [--perf-scope] [--out PATH]` | Build the autorun diagnostics image, flash, run every suite; 3000 s when omitted. |

Each prints the report and capture paths, PASS/FAIL counts, up to ten failure
messages (then a FAIL count per suite) and the end reason. `--verbose`
prints the whole capture; to find something in it, grep the capture instead.
One suite run once - `autana suite <name>` with no `--runs` or `--flash` -
still produces exactly one capture and one report, the same as before this
command absorbed `batch`.

**`--test PATTERN`** runs only the tests of a suite whose name contains
`PATTERN` (a case-sensitive substring; letters, digits and underscores). Repeat
the flag or comma-separate to pick several: `--test fire,gas` runs every test
whose name has `fire` or `gas` in it. The board does the filtering, so a test
that is not selected does not run, and a filtered capture is minutes instead of
a full suite's quarter hour. The limits (a pattern's length, how many) are the
board's, in `launcher/test/suites.h`; it refuses one past them. A pattern that
matches no test fails after the capture, listing the suite's test names, and
ends a `--runs` batch there; with a single pattern nothing ran. The report and
the `--runs` summary cover only the tests that ran - an unselected test is not
missing, failed or unmet - and name the filter, so an A/B read later knows
which rows it compares. It needs a diagnostics build whose `RUNSUITE` takes
patterns (`autana flash diag`), and says so when the board runs an older one.

`autana batch <suite>... [--runs N] [--perf-scope] [--verbose]` still works -
the old spelling of `autana suite <suite>... --runs N --perf-scope --verbose
--flash` (`--runs` defaults to 3 here, `suite`'s own default is 1). It prints
one line naming the new form, then runs it.

## Watch the board

`autana help watch`

| Command | What it does |
|---|---|
| `autana monitor [seconds] [--follow] [--stream] [--elf PATH]` | In a terminal: the console live, until Ctrl+C or for `seconds`. Piped or scripted: needs `seconds` or `--follow`, and prints only error lines and `FRAME_WATCH` warnings; `--stream` prints everything. Without a terminal and neither `seconds` nor `--follow`, this is a plain usage error rather than a hang. Always ends with its capture path. Crash addresses and `FRAME_WATCH` sites decode against `PATH`, or the build whose `build_id.txt` matches. |
| `autana reset [--capture [seconds]] [--verbose]` | Reboot and wait for USB serial. `--capture` records the boot (20 s) and prints its path and any error lines. |
| `autana screenshot [--as-shown\|--framebuffer] [-o PATH]` | `PATH.png` plus a `PATH.json` state snapshot. Landscape by default; `--as-shown` uses the board's orientation, `--framebuffer` the raw bytes. |
| `autana screenshot --frames N -o PATH` | `N` consecutive frames as `PATH-00` to `PATH-<N-1>`: one capture while running, then the loop frozen and stepped one frame between captures, then resumed. A band-mode capture shows the panel as it is, a band no frame resent included. |

## Drive input

`autana help input`

| Command | What it does |
|---|---|
| `autana tap <x> <y>` | Tap, 50 ms. |
| `autana press <x> <y> [ms]` | Hold; 1000 ms when omitted. |
| `autana drag <x0> <y0> <x1> <y1> <ms>` | Drag between two points over `ms`. |
| `autana button <boot\|power> [short\|long]` | A BOOT or PWR press; `short` when omitted. |

The two raw levels below gesture, `touch` and `imu`, live under
[`autana debug`](#debug).

## Apps

`autana help apps`

| Command | What it does |
|---|---|
| `autana apps [--json]` | The registered apps, and which is running. |
| `autana open <name>` | Enter an app, even while frozen; case-insensitive, unambiguous prefix. |
| `autana home` | Back to the launcher. |

## Tunables

`autana help tune` · what makes a constant tunable: [Live-Tuning.md](Live-Tuning.md)

| Command | What it does |
|---|---|
| `autana tune [text] [--json]` | List the tunables with their ranges; names containing `text`. |
| `autana tune <name> [value]` | Show one, or set it on the board (lost on reboot). |
| `autana tune reset <name>` | Back to the value the source declares. |
| `autana tune save` | Write the board's values into this project's `TUNE(...)` lines. |

## Sharing the board

`autana help lock` · busy board, guarantees and recovery: [Device-Lock.md](Device-Lock.md)

| Command | What it does |
|---|---|
| `autana status [--json]` | Every board, plugged in or locked: free or held, the holder with local start, elapsed and estimated free time, and the FIFO waiters with estimated starts. A board off USB is listed without a port. |
| `autana lock release [<token>]` | Release the lock a command of this session holds, before it would have. Given a lock's token, or, run from inside the command that holds the lock, without one. |
| `autana lock hand [--until-back <seconds>] <note...>` | Reserve the board for a person for an hour and emit `human-reserved`; running it again renews the hour, and an unrenewed reservation lapses. Refused while a command holds the board. With `--until-back`, wait until it is taken back or lapses. |
| `autana lock take-back` | Clear that reservation. |

A board is named by its USB serial number, so the lock follows it across
COM number changes. A command that loses the lock stops with `device lock was
lost`; one that finds the board busy exits 75
([exit codes](Device-Lock.md#exit-codes-and-json-status)). A separate `flash` and `suite` leave a gap where another session can
flash; `suite --flash` and `selftest` hold one lock across flash and capture.
Lock loss is defined in [Device-Lock.md](Device-Lock.md); flash success,
captures and wait estimates in [Flash-and-Captures.md](Flash-and-Captures.md).

The lock owner is set as described in [Flags](#flags) above.

`autana lock hand --until-back 30 put the board in download mode` pauses a flash
script until someone puts the board in download mode and runs `autana lock
take-back`. With `--until-back`, exit 0 means that reservation was released or
lapsed; the other outcomes are in [exit codes](Device-Lock.md#exit-codes-and-json-status).

## Debug

`autana help debug` - left out of the bare `autana help` listing; a session
rarely needs the frame loop paused or a raw sensor level, so these stay one
`autana help debug` away rather than crowding the everyday groups above.

| Command | What it does |
|---|---|
| `autana debug freeze` | Stop the frame loop where it is. |
| `autana debug resume` | Run it again. |
| `autana debug step [N]` | Advance `N` frames while frozen; 1 when omitted. |
| `autana debug touch <down\|up> <x> <y>` | One raw touch-controller level; `up` hands back to the controller. |
| `autana debug imu <ax> <ay> <az>` | Raw accelerometer counts; `autana debug imu release` hands back to the sensor. |
| `autana debug framewatch` | A development build's frame watch as JSON: the last frame's allocations, frees and log lines, and every site repeating frame after frame ([the frame watch](../Firmware-Architecture.md#the-frame-watch-no-allocating-or-logging-in-steady-state)). |

`autana freeze`, `autana resume`, `autana step`, `autana touch`, `autana imu`
and `autana framewatch` still work, each printing the new spelling once
before running it.

## No board needed

`autana help docs` · how it ranks and what it needs: [Docs-Search.md](Docs-Search.md)

| Command | What it does |
|---|---|
| `autana docs <question...>` | The three sections that answer it best, excerpted, each with its `path:line` range and the code it cites, then five more to read on. Needs no board. |
| `autana docs --section <path:line>` | One section whole, or `path#heading words`; `--deep` adds its subsections. |
| `autana docs --outline <path>` | A document's headings with their lines and sizes, to pick a section without reading the file. |
| `autana docs --ask <question...>` | A short answer written by the local chat model from those sections, with their sources. |
| `autana --version` (or `-V`) | This autana's own version - purely informational, rides along in a device lock record so a refusal can name what is holding the board; see [Device-Lock.md](Device-Lock.md#one-copy-of-the-tools). |

## JSON fields

| Command | Fields |
|---|---|
| `status` | `boards`, one object per board; the fields are in [Device-Lock.md](Device-Lock.md#exit-codes-and-json-status). |
| `buildid` | `build_id` |
| `apps` | `apps`: `name`, `running` |
| `suite list` | `suites`: `name`, `source`, `on_request`, `device_only` |
| `tune` | `tunables`: `name`, `value`, `min`, `max`, `default` |

A filter that matches nothing gives an empty array.

## In a session

A bare word is a command above; any other line goes to the board as typed,
so an app's own command works too. A tunable is only ever reached through
`tune`:

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
log and a manifest under the `records` setting, or the checkout's gitignored
`.records/device` without one
([Flash-and-Captures.md](Flash-and-Captures.md#where-a-capture-lands)).

## Settings

No environment variable is a setting; `_AUTANA_*` names are autana's own plumbing to its children, and it still reads system ones such as `IDF_TOOLS_PATH`. A setting is a global option before the
command, which covers that one call, or a key in `autana.local.toml` in the
project folder, which covers every command run from that checkout. The file is
optional and gitignored, and `autana help config` lists its keys.

| Global option | Sets |
|---|---|
| `--wait SECONDS` | How long a board command waits for the lock, across every step it runs: 600 s without it, `0` fails at once with exit 75. |
| `--owner NAME` | The name this run holds the lock under, as `NAME:<pid>`; `<user>@<host>:<pid>` without it. Name each CI job. |
| `--board SERIAL` | The board's USB serial number; without it, see [Which board](Device-Lock.md#which-board). |

All three go before the command (`autana --wait 0 monitor 5`); after it,
autana says so. A CI job that should fail rather than queue writes
`autana --wait 0 <command>`.

| Key | Meaning |
|---|---|
| `docs_extra` | A list of extra Markdown files or folders `autana docs` searches, relative to the project. |
| `records` | Where captures and `index.jsonl` land; `.records/device` in the checkout without it. |
| `lock_hook` | A shell command run on every device lock event ([events and environment](Device-Lock.md#lock-events)). |

```toml
docs_extra = ["notes/bench.md", "~/Documents/design-notes"]
records = "~/autana-records"
lock_hook = "python ~/bin/on_lock.py"
```

The file reads `key = "..."` lines (or `'...'` where a Windows path should stay
as typed) and one-line arrays of such strings, and refuses anything else and any
other key, naming the file and the line. The lock's folder and the docs model's
folder and port are deliberately not settings: they are machine facts shared by
every checkout.

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
- The handler gets only what follows the prefix: `autana example status`
  (or `example status` in a session) calls it with `"status"`.
- Every reply line starts with the prefix in capitals; the last is
  `<PREFIX>_END`, which lets autana return at once.
- Return `false` to decline; the shell answers `<PREFIX>_ERR not handled`.
  A prefix whose app is not running gets `<PREFIX>_ERR not running`.
- It runs on the frame loop after the app's `frame()`, and still runs while
  frozen. Lines faster than frames queue up to `APP_LINE_QUEUE_LEN`; one
  past that is dropped with no reply.
- No `#if` needed: `APP_CONSOLE_PTR()` is `NULL` in release, and the handler
  goes with it.

An app that adds a command documents it itself.

# The autana command

One command for everything that touches the board: build and flash, run
suites, watch it, drive its input, change a number live. It takes the device
lock and acts on the worktree you are standing in.

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
panel pixels. Tab completes command names, `flash` variants and help topics
where Python has `readline` (Windows: `pip install pyreadline3`).

## Build and flash

`autana help build`

| Command | What it does |
|---|---|
| `autana flash [rel\|dev\|diag] [--quiet] [--perf-scope]` | Build and flash this worktree; `dev` when omitted. `--quiet`: output to the log only. `--perf-scope` (diag): the perf-scoped image, no suite run. |
| `autana buildid [--json]` | The `BUILD_ID` the board is running, to check against what was flashed. |

After flashing, `autana flash` waits for USB Serial/JTAG to return, including
when Windows assigns a different COM number. It verifies the boot's `BUILD_ID`
against the build log. If the boot is not heard, it uses a watchdog reset and
checks again before returning. A missing or different id is reported explicitly.

## Tests

`autana help tests`

| Command | What it does |
|---|---|
| `autana suite <name> [seconds] [--verbose]` | Run one registered suite on a diagnostics build already on the board. |
| `autana suite list [text] [--json]` | The suites this worktree registers; `[on request]` ones run only by name. |
| `autana selftest [seconds] [--verbose]` | Build diagnostics+autorun, flash, run every suite; 3000 s when omitted. |
| `autana batch <suite>... [--runs N] [--perf-scope] [--verbose]` | Flash once, capture the suites `N` times (3) under one lock; one summary. |

Each prints the report and capture paths, PASS/FAIL counts, up to ten failure
messages (then a FAIL count per suite) and the end reason. `--verbose` prints the whole capture; to find
something in it, grep the capture instead.

## Watch the board

`autana help watch`

| Command | What it does |
|---|---|
| `autana monitor [seconds] [--follow] [--stream] [--elf PATH]` | In a terminal: the console live, until Ctrl+C or for `seconds`. Piped or scripted: needs `seconds` or `--follow`, and prints only error lines; `--stream` prints everything. Always ends with its capture path. Crash addresses decode against `PATH`, or the build whose `build_id.txt` matches. |
| `autana reset [--capture [seconds]] [--verbose]` | Reboot and wait for USB serial. `--capture` records the boot (20 s) and prints its path and any error lines. |
| `autana screenshot [--as-shown\|--framebuffer] [-o PATH]` | `PATH.png` plus a `PATH.json` state snapshot. Landscape by default; `--as-shown` uses the board's orientation, `--framebuffer` the raw bytes. |

## Drive input

`autana help input`

| Command | What it does |
|---|---|
| `autana tap <x> <y>` | Tap, 50 ms. |
| `autana press <x> <y> [ms]` | Hold; 1000 ms when omitted. |
| `autana drag <x0> <y0> <x1> <y1> <ms>` | Drag between two points over `ms`. |
| `autana touch <down\|up> <x> <y>` | One raw touch-controller level; `up` hands back to the controller. |
| `autana imu <ax> <ay> <az>` | Raw accelerometer counts; `autana imu release` hands back to the sensor. |
| `autana button <boot\|power> [short\|long]` | A BOOT or PWR press; `short` when omitted. |

## Apps

`autana help apps`

| Command | What it does |
|---|---|
| `autana apps [--json]` | The registered apps, and which is running. |
| `autana open <name>` | Enter an app, even while frozen; case-insensitive, unambiguous prefix. |
| `autana home` | Back to the launcher. |

## Frame loop

`autana help frames`

| Command | What it does |
|---|---|
| `autana freeze` | Stop the frame loop where it is. |
| `autana resume` | Run it again. |
| `autana step [N]` | Advance `N` frames while frozen; 1 when omitted. |

## Tunables

`autana help tune` · what makes a constant tunable: [Live-Tuning.md](Live-Tuning.md)

| Command | What it does |
|---|---|
| `autana tune [text] [--json]` | List the tunables with their ranges; names containing `text`. |
| `autana tune <name> [value]` | Show one, or set it on the board (lost on reboot). `trail` works for `ridge.trail` when unambiguous. |
| `autana tune reset <name>` | Back to the value the source declares. |
| `autana tune save` | Write the board's values into this worktree's `TUNE(...)` lines. |

## Sharing the board

`autana help lock` · the lock itself: [Device-Lock.md](Device-Lock.md)

| Command | What it does |
|---|---|
| `autana status [--json]` | Who holds the board, and who is waiting. |
| `autana id [--json]` | The name this session holds the lock under: `autana-cli@<pid in base36>`. |
| `autana release <token>` | Release a lock this session holds; the token is what its command printed. |
| `autana hand [--wait <seconds>] <note...>` | Reserve the board and emit `human-reserved`; with `--wait`, wait until `take-back` emits `human-cleared`. |
| `autana take-back` | Clear that reservation. |

`autana hand --wait 30 put the board in download mode` pauses a flash script
until someone puts the board in download mode and runs `autana take-back`.
With `--wait`, exit 0 means that reservation was released. Exit 3 means the
wait timed out or was interrupted with Ctrl+C; the reservation stays. Exit 4
means another hand replaced the reservation; that reservation stays. The
caller decides how to proceed after either nonzero result.

## JSON fields

| Command | Fields |
|---|---|
| `status` | `state` (`unlocked`, `held`, `human`), `waiting`; held: `owner`, `purpose`, `acquired_at`; human: `owner`, `note`, `age_seconds` |
| `buildid` | `build_id` |
| `id` | `owner`, `pid` |
| `apps` | `apps`: `name`, `running` |
| `suite list` | `suites`: `name`, `source`, `on_request`, `device_only` |
| `tune` | `tunables`: `name`, `value`, `min`, `max`, `default` |

A filter that matches nothing gives an empty array.

## In a session

A bare word is a command above; any other line goes to the board as typed,
so an app's own command works too. A tunable is only ever reached through
`tune`:

```
autana> tune trail 200
autana> freeze
autana> step 3
autana> resume
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
log and a manifest under `AUTANA_RECORDS` - the checkout's gitignored
`.records/device` when unset, `.dev/records/device` when the PATH installer
finds a `.dev` checkout beside it.
Set `AUTANA_LOCK_HOOK` to a shell command for lock events; see
[Device-Lock.md](Device-Lock.md#lock-events) for events and variables.

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

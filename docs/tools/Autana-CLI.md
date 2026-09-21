# The autana command

One terminal command for everything that touches the board: flashing it,
reading what it says, running a suite on it, changing a number on it,
standing in for a touch or an IMU sample, capturing what the panel shows.
No `idf.py` environment activation, and no remembering which script under
which folder does which half.

`autana` with no arguments opens a session; every command below also works
as a one-shot from the shell.

```
$ autana
autana console - 'help' for the commands, 'quit' to leave
autana> buildid
```

## Commands

| | |
|---|---|
| `autana flash [rel\|dev\|diag] [--quiet]` | Build and flash the worktree you are standing in, `dev` when the variant is omitted. Output streams to the terminal; `--quiet` leaves it in the log file only. |
| `autana monitor [seconds]` | Print what the board says, for 60 seconds when omitted. |
| `autana suite <name> [seconds]` | Run one registered suite and print what it prints. |
| `autana suite list [text]` | The suites this worktree registers, read from its sources; `[text]` keeps the names containing it. |
| `autana tune [text]` | The numbers a development build lets you change, with their ranges; `[text]` keeps the names containing it. |
| `autana tune <name>` | One of them, when the name is exactly one tunable's own (owner optional when unambiguous); the same filtered listing as `[text]` otherwise. |
| `autana tune <name> <value>` | Change one on the running device. |
| `autana tune reset <name>` | Back to the value the source declares. |
| `autana tune save` | Write the device's current values into the `TUNE(...)` lines of the worktree you are standing in. |
| `autana screenshot [-o PATH]` | What the panel shows right now, as `PATH.png` plus a `PATH.json` state snapshot; `PATH` defaults to a timestamped name in the current directory. |
| `autana freeze` | Stop the frame loop where it is. |
| `autana resume` | Let the frame loop run again. |
| `autana step [N]` | Advance N frames while frozen, 1 when `N` is omitted. |
| `autana touch <down\|up> <x> <y>` | Stand in for the touch controller. |
| `autana imu <ax> <ay> <az>` | Stand in for the IMU, raw accelerometer counts. |
| `autana buildid` | The `BUILD_ID` the board answers with, so what is running can be checked against what was flashed. |
| `autana id` | The name this `autana` holds the board under, and its pid: `autana-cli@<pid in base36>`. |
| `autana help` | The same list. |

Inside a session the `autana` prefix is dropped, but tuning stays explicit -
a bare word is either one of the commands above or a device console verb
(`screenshot`, `freeze`, ...), never an implicit tunable lookup:

```
autana> tune wave
autana> tune trail                # show
autana> tune trail 200            # change
autana> freeze
autana> step 3
autana> resume
autana> quit
```

A tunable's name may drop its owner when that is unambiguous: `trail` for
`ridge.trail`. See [Live-Tuning.md](Live-Tuning.md) for what makes a
constant tunable in the first place.

**A development build is what answers.** A release image has no console
listener, no registry and no names - `buildid`, `tune` and the rest have
nothing to talk to. See [../Build-Variants.md](../Build-Variants.md).

## Where it lives

| | |
|---|---|
| `tools/autana`, `tools/autana.cmd` | The launchers. `tools/` is what goes on the PATH. |
| `scripts/autana/autana.py` | Every command. |
| `scripts/device/device.py` | The serial port and the device lock. Nothing else opens the port. |
| `scripts/add-tools-to-path.sh` | Puts `tools/` on the PATH, and proves a new terminal finds it. |

```sh
scripts/add-tools-to-path.sh            # add, then verify
scripts/add-tools-to-path.sh --check    # verify only, change nothing
```

Run the installer from the **primary checkout**, not a worktree: a worktree
is deleted sooner or later and would leave a dead PATH entry. One entry
serves every worktree, because what a command acts on is the worktree you
are standing in - found from the working directory - not the checkout the
command itself came from.

## The device lock, and records

Every command that touches the board goes through `device.py`, which takes
the device lock first, so two sessions cannot drive one board at once.
`autana monitor` shows who is waiting; `autana id` names this session in
that queue. The lock lives in the system temp folder, so it is one lock per
machine rather than one per checkout. See `.dev/docs/workflows/Device-Lock.md`.

Each session writes a log and a manifest. `AUTANA_RECORDS` names where;
unset, they land in the checkout's own gitignored `.records/device`. The
PATH installer sets it to `.dev/records/device` when a `.dev` checkout sits
beside this one, which is where this project keeps and tracks its device
history.

## An app's own verbs

The commands above are the CLI's, and cover most of what the console
answers directly: `screenshot`, `freeze`, `resume`, `step`, `touch`, `imu`
and the tuning verbs all go through `device.py`'s lock the same way. A verb
the CLI has no command for - `runsuite` among them, since `autana suite`
already runs one and reports the result - is still only a line away: type
it in an interactive session, or extend `autana` here. The verbs themselves
are a separate list, one file each under `launcher/main/console/`.

An app can answer its own commands too, without joining that list: set
`console_line` in its `app_t` (`launcher/main/app.h`). A line none of the
verbs above claims reaches the running app's callback instead of being
logged and dropped - match the line, act, and return true to claim it.
Built-in verbs still take precedence, so an app can never shadow one, and
an unclaimed line only ever reaches whichever app is currently running,
never the launcher. The callback runs on the frame loop, not the console's
own reader task, so it can `printf()` a reply directly. Sending one is the
same as any other line: type it in an interactive `autana` session. An app
that adds a command documents it itself - see `docs/sand/` for `counts`.

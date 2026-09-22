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
| `autana flash [rel\|dev\|diag] [--quiet] [--perf-scope]` | Build and flash the worktree you are standing in, `dev` when the variant is omitted. Output streams to the terminal; `--quiet` leaves it in the log file only. `--perf-scope`, with `diag`, builds the perf-scoped image and leaves it on the board with no suite run. |
| `autana monitor [seconds] [--elf PATH]` | Print what the board says, for 60 seconds when omitted. Any crash address seen is decoded against `PATH`'s symbols; with no `PATH`, the build directory whose own `build_id.txt` matches the capture's `BUILD_ID`, if one does. |
| `autana reset [--capture [seconds]]` | Reboot the board and wait for its USB serial port to return. `--capture` also prints and records the boot console, for 20 seconds when omitted; what the board prints while USB re-enumerates may be lost. |
| `autana suite <name> [seconds]` | Run one registered suite and print what it prints. |
| `autana suite list [text]` | The suites this worktree registers, read from its sources; `[text]` keeps the names containing it. |
| `autana selftest [seconds]` | Build the diagnostics+autorun image and run every suite this worktree registers, on the device - 3000 seconds when omitted; can take minutes. |
| `autana batch <suite> [<suite> ...] [--runs N] [--perf-scope]` | Flash the diagnostics image once and capture the given suites `--runs` times (3 when omitted) under one lock, so no other session can flash between two captures of the same image; writes one summary across every run. |
| `autana tune [text]` | The numbers a development build lets you change, with their ranges; `[text]` keeps the names containing it. |
| `autana tune <name>` | One of them, when the name is exactly one tunable's own (owner optional when unambiguous); the same filtered listing as `[text]` otherwise. |
| `autana tune <name> <value>` | Change one on the running device. |
| `autana tune reset <name>` | Back to the value the source declares. |
| `autana tune save` | Write the device's current values into the `TUNE(...)` lines of the worktree you are standing in. |
| `autana screenshot [-o PATH]` | What the panel shows right now, as `PATH.png` plus a `PATH.json` state snapshot; `PATH` defaults to a timestamped name in the current directory. |
| `autana freeze` | Stop the frame loop where it is. |
| `autana resume` | Let the frame loop run again. |
| `autana step [N]` | Advance N frames while frozen, 1 when `N` is omitted. |
| `autana touch <down\|up> <x> <y>` | Stand in for the touch controller - QEMU images only (`console_inject.c` compiles under `CONFIG_LAUNCHER_QEMU`), no real controller to override on the board. |
| `autana imu <ax> <ay> <az>` | Stand in for the IMU, raw accelerometer counts - QEMU images only, same reason. |
| `autana buildid` | The `BUILD_ID` the board answers with, so what is running can be checked against what was flashed. |
| `autana id` | The name this `autana` holds the board under, and its pid: `autana-cli@<pid in base36>`. |
| `autana status` | Who, if anyone, holds the board right now, and who else is waiting. |
| `autana release <token>` | Release a lock this session holds, before its own command would have - the token is what that command printed when it acquired it. |
| `autana hand <note>` | Reserve the board for a maintainer sitting at it; `autana` refuses new work against it until `take-back`. |
| `autana take-back` | Clear a reservation `hand` made, freeing the board again. |
| `autana help` | The same list. |

Inside a session the `autana` prefix is dropped, but tuning stays explicit -
a bare word is one of the commands above, or the whole line is sent to the
board as typed - never an implicit tunable lookup:

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
machine rather than one per checkout. See [Device-Lock.md](Device-Lock.md).

Each session writes a log and a manifest. `AUTANA_RECORDS` names where;
unset, they land in the checkout's own gitignored `.records/device`. The
PATH installer sets it to `.dev/records/device` when a `.dev` checkout sits
beside this one, which is where this project keeps and tracks its device
history.

## Adding a command from an app

The commands above are the CLI's own; the verbs they send (`screenshot`,
`freeze`, `set`, ...) are a separate list, one file each under
`launcher/main/console/`. An app can answer commands of its own without
joining that list:

1. Declare a prefix with `APP_CONSOLE()`, once, before the app's own
   `app_t` - non-empty and checked at compile time; a clash with a verb or
   another app's prefix, or a prefix carrying a space of its own, is
   checked again at boot, so two commands can never claim the same line:

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

   No `#if CONFIG_LAUNCHER_DEVELOPMENT` needed anywhere in the app -
   `APP_CONSOLE_PTR()` is `NULL` in a release build, and the handler goes
   with it.
2. The shell matches the prefix, a whole word, and calls the handler with
   only what follows it (`args`, `""` for a bare prefix) - never the prefix
   itself. `example status` above is typed in full; the handler only ever
   sees `"status"`.
3. Reply with `printf()`: every line starts with the prefix in capitals,
   and the last one is `<PREFIX>_END` - autana returns as soon as that line
   arrives rather than waiting out a timeout, the same way `TUNE_END`
   already lets `autana tune` return early.
4. Return `true` to claim it. Returning `false` need not reply itself - the
   shell sends `<PREFIX>_ERR not handled` on the handler's behalf.

Send it the same way as any built-in verb, one-shot (`autana example
status`) or typed inside a session (`example status`).

A few rules that follow from the shape above:

- A registered verb always wins, so an app can never shadow one; a clash
  between a prefix and a verb, or between two apps' own prefixes, fails the
  boot loudly rather than silently losing one of them.
- A line whose prefix belongs to an app that is not the one running gets
  `<PREFIX>_ERR not running`, not silence.
- The handler runs on the frame loop, after the app's own `frame()`, so it
  sees settled state - and still runs while the frame loop is frozen
  (`autana freeze`), the obvious use being freeze, inspect, step.
- Lines arriving faster than frames queue, up to `APP_LINE_QUEUE_LEN`
  (`console.c`); one past that is dropped and logged on the device side,
  with no reply at all - the caller sees whatever the short window catches.

An app that adds a command documents it itself.

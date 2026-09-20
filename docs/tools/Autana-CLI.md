# The autana command

One terminal command for everything that touches the board: flashing it,
reading what it says, running a suite on it, changing a number on it. No
model tokens, no `idf.py` environment activation, and no remembering which
script under which folder does which half.

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
| `autana get <name>` | One of them. |
| `autana set <name> <value>` | Change one on the running device. |
| `autana reset <name>` | Back to the value the source declares. |
| `autana save` | Write the device's current values into the `TUNE(...)` lines of the worktree you are standing in. |
| `autana buildid` | The `BUILD_ID` the board answers with, so what is running can be checked against what was flashed. |
| `autana id` | The name this `autana` holds the board under, and its pid: `autana-cli@<pid in base36>`. |
| `autana help` | The same list. |

Inside a session the `autana` prefix is dropped, and a tunable's name alone
is `get`, a name and a value `set`:

```
autana> tune wave
autana> trail                     # get
autana> trail 200                 # set
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

The commands above are the CLI's. The verbs the firmware answers on the
console - `screenshot`, `runsuite`, `freeze`, `step`, `set` and the rest -
are a separate list, one file each under `launcher/main/console/`, and an
app that adds its own documents them itself. `autana` does not need to know
about a verb to carry a line to the board.

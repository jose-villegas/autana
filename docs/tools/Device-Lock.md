# Device lock

The shared board has one USB serial port. Nothing opens that port except
`scripts/device/device.py`: no monitor, capture helper, `esptool`, or direct
pyserial command. The tool finds the board by USB Serial/JTAG VID `0x303A`,
not a fixed COM number, and opens it at 115200 with DTR and RTS low.

The lock is keyed by the board's USB Serial/JTAG identity. Its COM number
can change after a flash or reset; each port access resolves the current
number. The flash script checks the live lock token just before `idf flash`.
Esptool hash-verifies the written regions. `batch` and `selftest` keep one
lock across the flash and capture.

Day-to-day interactive use goes through `tools/autana`
([Autana-CLI.md](Autana-CLI.md)) - every `autana` command calls `device.py`
for the lock and the port. This doc covers `device.py` itself: its own
command-line shape, for a script that names its own `--owner`/`--purpose`
rather than `autana`'s generated one, and recovery when a lock will not let
go.

Run the tool with ESP-IDF's Python (the `python.exe` under
`%USERPROFILE%\.espressif\python_env\idf<version>_py<version>_env\Scripts\`
on Windows), so its pyserial installation is available; the examples below
write it as `python`.
It works the same from PowerShell, cmd or Git Bash: `flash`, `batch`, and
`selftest` run `build_flash.sh` with Git for Windows' own `bash.exe`, never
whatever `bash` is first on `PATH` - from a native shell that is WSL's
launcher, which strips the backslashes out of the script path and could
not run ESP-IDF anyway.

Every command but `report` takes `--owner` (defaults to
`AUTANA_DEVICE_OWNER`) and its own `--purpose` (each subcommand has a
sensible default - `flash`, `reset`, `run suite`, `send`, `screenshot`,
`batch capture` - override it to say why on a shared board).

```powershell
python scripts/device/device.py status
python scripts/device/device.py --owner sam flash --variant dev --worktree C:\path\to\engine
python scripts/device/device.py --owner sam run-suite sand --expect-build-id 0123456789ab-dev
python scripts/device/device.py --owner sam listen --seconds 30
```

`listen` takes `--follow` to run until Ctrl+C and `--echo` to print the full stream.

### Talking to a running device: `send`

`send` writes one console line and prints the device's replies to it, under
the lock like everything else. It is what live tuning uses - the firmware's
`util/tune` answers `SET <name> <value>`, `GET <name>` and `TUNE` on a
development build - and what `autana tune` calls:

```powershell
python scripts/device/device.py --owner maintainer send "SET ridge.trail 200"
python scripts/device/device.py --owner maintainer send TUNE
```

A reply is everything from `--reply` (default `TUNE`) to the end of its
line, since the console also carries the firmware's log lines; the answer
ends at a line starting with one of `--until` (default `TUNE_OK`, `TUNE_ERR`,
`TUNE_END`). It exits 1 on an `_ERR` reply, and says so when the build does
not know the command or nothing answers within `--seconds` (default 3).

`send` is not a capture: it adds no capture record to `index.jsonl`. Its
duration is recorded with every other held command in `durations.jsonl`.

### Screenshots: `device.py screenshot`

`device.py screenshot [--as-shown|--framebuffer] [--out PATH]
[--timeout SECONDS]` takes the lock,
requests the panel capture and writes a `.png` plus a `.json` state snapshot;
`autana screenshot` calls it the same way. A script that needs its own
`--owner`/`--purpose` calls `device.py screenshot` directly, the same way
it calls `run-suite` rather than `autana suite`. The wire protocol and the
BMP-to-PNG decoder
live in `launcher/tools/device/screenshot.py`, imported as a library - it opens no
port itself.

### Measuring: use `batch`, not a sequence of commands

A measurement is `batch`: it takes the lock ONCE, builds and flashes once,
captures every suite `--runs` times, and writes one summary across all runs.
Holding the board for the whole sequence means nobody else can flash
between two captures of the same image, and the command blocks until it is
done. `autana batch` calls
it the same way ([Autana-CLI.md](Autana-CLI.md)); a script with its
own `--owner`/`--purpose` calls `device.py batch` directly:

```powershell
python scripts/device/device.py --owner sam batch --worktree C:\path\to\engine --suite run_sand_perf_suite --suite run_gfx_suite --runs 3
```

The summary (`<HHMMSS>_batch_<owner>.md` in the day's records folder) shows,
per suite: every timing per run with min, max and spread; every test whose
result CHANGED between runs of the image - a test that flaps on one binary is
a finding, not noise; the tests that failed in every run; and each
`PERF TARGET` per run. A capture that errors is recorded and the batch
continues; only a failed build or flash stops it. `--perf-scope` builds the
perf-scoped image; build_flash.sh itself refuses an unsupported flag rather
than silently building the full image. `--out PATH` writes the one raw
capture to `PATH` instead of the default path - only with exactly one
`--suite` and `--runs 1`, which is how `device_report.sh`'s RUNSUITE-scoped
reports (report_boot_anim_perf.sh) call it.

A separate `flash` and `run-suite` take separate locks. Another session can
flash between them; the capture fails if it sees a different `BUILD_ID`.
Use `batch` or `selftest` when flash and capture must share one lock.

`flash`, `run-suite`, `selftest`, `batch`, `listen`, `reset`, `send`, and
`screenshot` take the lock before they touch the board, keep it through their
whole operation, and renew it every 30 seconds. Serial opens and esptool calls
check the active lock token; the flash script checks it against the lock file
before invoking `idf flash`. `reset` reboots with esptool and returns once the
port is back. `reset --capture` and `selftest` reopen the port for capture if it
vanishes or remains silent after a reset.

`flash` trusts esptool's hash check of every region in `write_flash`. The
`BUILD_ID` in `build_id.txt` must match the build log. Flash success proves
the write, not boot. A release flash has no boot verification. A later
`selftest` or `batch` capture checks the console `BUILD_ID` against the
flashed image. `selftest` builds the diagnostics+autorun image and
captures the boot-time run until `SELFTEST_COMPLETE`; report scripts use the
same held lock for their flash and capture. The default lock wait is ten
minutes; pass `--wait 0` to return immediately when the board is busy.

`run-suite` stops at the shell's `RUNSUITE_COMPLETE name=<suite>` line (or an
older build's `SUITE_DONE`), or after its non-`shell:` output is idle. A port
that disappears mid-capture ends it as `port lost` with what was read kept, so
a `batch` carries on with its next suite. It fails if the build has no suites,
does not contain the requested suite, or reports any failed test. It prints
the number of `PASS` and `FAIL` result lines when the capture ends, and fails
if a different `BUILD_ID` appears while capturing.

### Where a capture lands

No capture command needs `--out`: by default each writes to
`<records>/<YYYYMMDD>/<HHMMSS>_<kind>_<owner>.log` (`kind` is
`flash-<variant>`, `reset`, `runsuite-<suite>`, `selftest`, or `listen`). `<records>` is
`$AUTANA_RECORDS` when set, otherwise the checkout's own gitignored
`.records/device`, so nothing a commit can pick up by accident; the
maintainer's shell points `AUTANA_RECORDS` at the `.dev` checkout, where the
device history is tracked. A default-path capture over 200 KB is gzipped in
place (suite/listen captures rarely reach that; a flash log at ~270 KB usually
does); pass `--out <path>` to any of them to write exactly
there instead, uncompressed, e.g.:

```powershell
python scripts/device/device.py --owner sam run-suite sand --out C:\Temp\sand.log
python scripts/device/device.py --owner sam listen --seconds 30 --out C:\Temp\listen.log
```

Every invocation - default path or explicit `--out`, success or failure -
also appends one line to `<records>/index.jsonl`: owner, purpose, port,
the command and its arguments, the build id (from the matched build log
for flash, observed in a capture otherwise), lock acquisition time, the
worktree and commit involved, how the capture ended, and any error.
`durations.jsonl` holds command durations. `device.py` never commits
these records; whoever ran the command commits the evidence with the work.

`run-suite` also writes a parsed `<same stem>.md` beside its capture -
suite PASS/FAIL counts, every failing test's Unity message, and any
`PERF TARGET` lines - so a result is readable without opening the raw log.
When the manifest's `worktree` names a checkout with exactly one app whose
`tools/report_performance.py` registers the suite that ran
(`SUITE_REGISTER(<suite>)` in that app's own `tests/suite_*.c`), that app's
frame-budget table is appended too; zero or several matches, or a reporter
that fails, are noted in the report instead of the table - a capture is
never failed over this. Rebuild a report for any existing capture without
re-running anything:

```powershell
python scripts/device/device.py report .records/device/20260916/153113_runsuite-run_sand_perf_suite_sam.log
```

`report` touches no lock and no port - it only reads a capture and
`index.jsonl` already on disk, so it works on any capture, including a
`flash` or `listen` one (both of these narrower reports: no suite was run,
so there is no PASS/FAIL table or app budget table to build).

The lock state is `%TEMP%/autana-device/usb-303a.json`. It is JSON containing
`owner`, `purpose`, `kind`, `port`, `acquired_at`, `heartbeat_at`,
`expected_build_id`, `host`, `pid`, and an opaque `token`. `port` records
the COM number found when the lock was taken; it is not the lock key.
FIFO waiter tickets live in the matching `.queue/` directory. A dead
waiter is discarded.
A lock is reclaimed when its heartbeat is more than ten minutes old, or when
its holder's process on this host is dead; the acquirer logs
`reclaimed lock from <owner> for <purpose> (heartbeat expiry | dead process)`.
A process counts as dead only when the process table proves it: on Windows
`OpenProcess`/`GetExitCodeProcess`, never `os.kill(pid, 0)`, whose signal 0
is `CTRL_C_EVENT` there and misreports any process on another console. After
winning the lock a command also waits for the serial port itself to come
free, since a previous holder's reader can outlive its lock.

`status` shows the holder's local start time and elapsed time, an estimated
free time, and every waiter's purpose and estimated start in FIFO order.
`durations.jsonl` records the time from each held command's own start to
its end, including nested flash and suite commands and commands without
captures. Estimates use the median of the last 30 successful durations
for the same command kind, after at least three runs. Missing history,
a human reservation, or an unknown preceding duration makes the affected
estimate `unknown`. A holder past its median is estimated free now.
A waiting command prints its queue place and estimated start.
`started_at` in a capture record is the invocation time; `acquired_at`
is when the outer lock was won. The flash script's token check covers the
instant before `idf flash`; it cannot prove continued ownership during
the esptool operation. Port opens and captures stop with `device lock was
lost` if ownership changes.
`build_flash.sh --check-flash-lock <PORT>` runs the token check without
building or flashing; it uses the held token in
`AUTANA_DEVICE_LOCK_TOKEN`.

`device.py --owner <owner> release --token <token>` releases a lock this
owner holds without touching the board - useful when a run finishes
early and wants to free the port for the next waiter immediately rather
than waiting out a command's own lifetime. `autana release <token>` calls it
the same way.

For inspection or emergency recovery, use the lower-level command:

```powershell
python scripts/device/device_lock.py --port COM5 status
python scripts/device/device_lock.py --port COM5 acquire --owner sam --purpose investigate --wait 60
python scripts/device/device_lock.py --port COM5 heartbeat --token <token>
python scripts/device/device_lock.py --port COM5 release --token <token>
```

The acquire result prints the token as JSON. Releasing requires that token, so
one owner cannot release another owner's active lock.

To reserve the board, record the reservation before using it -
`autana hand <note>`, or `hand-to-human` directly for an active lock's own
token:

```powershell
python scripts/device/device.py --owner maintainer hand-to-human --token <token> --note "checking the panel"
```

When a session holds the board, its token releases that lock before the command
creates the human reservation. Without an active lock, omit `--token` (what
`autana hand` always does). The reservation appears in `status` and prevents
future acquisitions. When the reservation is no longer needed, clear it -
`autana take-back`, or:

```powershell
python scripts/device/device.py --port COM5 --owner maintainer take-back
```

This clears the reservation and prints the resulting lock status. The lower-level
`device_lock.py --port COM5 clear-human` remains available
for recovery.

## Lock events

Set `AUTANA_LOCK_HOOK` to a shell command to run when the lock changes. The
command receives these environment variables: `AUTANA_LOCK_EVENT`,
`AUTANA_LOCK_PORT`, `AUTANA_LOCK_OWNER`, `AUTANA_LOCK_PURPOSE`, and
`AUTANA_LOCK_NOTE`. Purpose is empty for human reservations; note carries the
reclaim reason for `lost` and the reservation note for human events. The
command runs through `cmd.exe` on Windows (`%VAR%`) and
`/bin/sh` elsewhere (`$VAR`); a script that reads the variables works on both.
Hooks run in separate processes and are not ordered across them, so one
holder's `released` can arrive after the next holder's `acquired`. A hook has
a three second timeout. A failed or timed out hook is quiet and never
changes the lock operation's outcome.

| Event | When |
|---|---|
| `acquired` | A ticket takes the lock, including reclaiming a stale lock. |
| `released` | The holder gives up the lock. |
| `waiting` | A ticket begins a real wait for a held or reserved board; once per ticket. |
| `gave-up` | A waiting ticket leaves without the lock. |
| `human-reserved` | A human reservation is recorded. |
| `human-cleared` | A human reservation is cleared. |
| `lost` | A stale lock is reclaimed; owner and purpose identify its former holder, and note gives the reclaim reason. |

For example, set the hook to `python path/to/board-events.py` and give that
script either job:

- Tell you the board is free when `AUTANA_LOCK_EVENT` is `released` or
  `human-cleared`.
- Append `AUTANA_LOCK_EVENT`, `AUTANA_LOCK_OWNER`, `AUTANA_LOCK_PORT`, and
  `AUTANA_LOCK_PURPOSE` to a usage log.

The recovery command `device_lock.py` also emits lock and reservation events.

`autana hand --wait <seconds> <note...>` waits without holding the device
lock; `human-reserved` fires when the reservation is recorded and
`human-cleared` when it is released. The wait loop emits no additional event.
Release returns 0, timeout or Ctrl+C returns 3, and a replacement reservation
returns 4 without clearing it.

## Related

- [Autana-CLI.md](Autana-CLI.md) - the interactive `autana` command built on
  top of this lock.
